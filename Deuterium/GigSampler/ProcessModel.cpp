#include "ProcessModel.hpp"

#include <Audio/Settings/Model.hpp>
#include <Process/Dataflow/Port.hpp>

#include <score/application/ApplicationContext.hpp>
#include <score/tools/ThreadPool.hpp>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/ProcessMetadata.hpp>

#include <wobjectimpl.h>
W_OBJECT_IMPL(Deuterium::Gig::ProcessModel)

namespace Deuterium::Gig
{

ProcessModel::ProcessModel(
    const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
    QObject* parent)
    : Process::
          ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , midi_in{std::make_unique<Process::MidiInlet>("MIDI In", Id<Process::Port>(0), this)}
    , audio_out{std::make_unique<Process::AudioOutlet>(
          "Audio Out", Id<Process::Port>(0), this)}
{
  metadata().setInstanceName(*this);

  m_inlets.push_back(midi_in.get());
  for(auto* control : makeSamplerControls(this))
    m_inlets.push_back(control);
  m_outlets.push_back(audio_out.get());
  ((Process::AudioOutlet*)audio_out.get())->setPropagate(true);

  // Empty or unparseable data yields a valid, silent process (no file loaded)
  // rather than failing construction; the path is kept so that saving the
  // document does not lose the reference.
  const auto parsed = parseInstrumentPath(data);
  m_filePath = parsed.file;
  m_instrument = parsed.instrument;
  if(!m_filePath.isEmpty())
  {
    // Phase 1: fast metadata parse on the GUI thread
    m_gigInfo = loadGigFileMetadata(m_filePath, m_instrument);

    // Phase 2: async sample loading on worker thread
    if(m_gigInfo)
      startAsyncSampleLoad();
  }
}

ProcessModel::~ProcessModel()
{
  // Cancel any in-flight loading
  if(m_cancelToken)
    m_cancelToken->store(true, std::memory_order_relaxed);
}

QString ProcessModel::effect() const noexcept
{
  return m_filePath;
}

void ProcessModel::loadFile(const QString& data)
{
  const auto parsed = parseInstrumentPath(data);
  loadFile(parsed.file, parsed.instrument);
}

void ProcessModel::loadFile(const QString& path, int instrument)
{
  // Cancel any in-flight loading
  if(m_cancelToken)
    m_cancelToken->store(true, std::memory_order_relaxed);

  // Keep the path even when loading fails (missing file, unmounted drive...)
  // so that re-saving the document does not erase the reference.
  m_filePath = path;
  m_instrument = instrument;
  m_gigInfo.reset();

  if(!path.isEmpty())
  {
    // Phase 1: fast metadata parse
    m_gigInfo = loadGigFileMetadata(path, instrument);
  }

  if(m_gigInfo)
  {
    // Phase 2: async sample loading; fileChanged() is emitted once samples land
    startAsyncSampleLoad();
  }
  else
  {
    // Loading failed: notify so any executor stops playing stale data
    fileChanged();
  }
}

void ProcessModel::startAsyncSampleLoad()
{
  auto rate = score::AppContext().settings<Audio::Settings::Model>().getRate();

  // Create a new cancellation token for this load
  auto cancelToken = std::make_shared<std::atomic<bool>>(false);
  m_cancelToken = cancelToken;

  // Capture metadata snapshot for the worker thread
  auto metadataSnapshot = m_gigInfo;

  // Use a QPointer so the callback is safe if ProcessModel is deleted
  QPointer<ProcessModel> self = this;

  score::TaskPool::instance().post(
      [metadataSnapshot, rate, cancelToken, self]() mutable {
        // Phase 2: load all sample data (slow)
        auto loaded = loadGigFileSamples(metadataSnapshot, rate, cancelToken);

        if(cancelToken->load(std::memory_order_relaxed))
          return;

        // Signal back to GUI thread via QCoreApplication context.
        // The QPointer 'self' is only dereferenced on the GUI thread.
        // On failure, fileChanged() is still emitted so that the executor
        // stops playing the previous file's samples.
        QMetaObject::invokeMethod(
            qApp,
            [cancelToken, self, loaded = std::move(loaded)]() mutable {
              if(cancelToken->load(std::memory_order_relaxed))
                return;
              if(!self)
                return;

              if(loaded)
                self->m_gigInfo = std::move(loaded);
              self->fileChanged();
            },
            Qt::QueuedConnection);
      });
}

}
