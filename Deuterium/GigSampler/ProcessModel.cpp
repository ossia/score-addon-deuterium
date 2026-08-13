#include "ProcessModel.hpp"

#include <Audio/Settings/Model.hpp>
#include <Process/Dataflow/Port.hpp>

#include <score/application/ApplicationContext.hpp>
#include <score/tools/ThreadPool.hpp>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

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
    , m_filePath{data}
{
  metadata().setInstanceName(*this);

  // Phase 1: fast metadata parse on the GUI thread
  m_gigInfo = loadGigFileMetadata(data);
  if(!m_gigInfo)
    throw std::runtime_error("Could not load GIG/DLS/SF2 file");

  m_inlets.push_back(midi_in.get());
  m_outlets.push_back(audio_out.get());
  ((Process::AudioOutlet*)audio_out.get())->setPropagate(true);

  // Phase 2: async sample loading on worker thread
  startAsyncSampleLoad();
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

void ProcessModel::loadFile(const QString& path)
{
  // Cancel any in-flight loading
  if(m_cancelToken)
    m_cancelToken->store(true, std::memory_order_relaxed);

  // Phase 1: fast metadata parse
  auto info = loadGigFileMetadata(path);
  if(info)
  {
    m_gigInfo = info;
    m_filePath = path;

    // Phase 2: async sample loading
    startAsyncSampleLoad();
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
        {
          qWarning() << "GigSampler: load cancelled";
          return;
        }

        if(!loaded)
        {
          qWarning() << "GigSampler: loadGigFileSamples returned null!";
          return;
        }

        // Signal back to GUI thread via QCoreApplication context.
        // The QPointer 'self' is only dereferenced on the GUI thread.
        QMetaObject::invokeMethod(
            qApp,
            [cancelToken, self, loaded = std::move(loaded)]() mutable {
              if(cancelToken->load(std::memory_order_relaxed))
                return;
              if(!self)
                return;

              auto& instr = loaded->instruments[loaded->selectedInstrument];
              qWarning() << "GigSampler: GUI callback firing."
                         << "instrument:" << instr.name.c_str()
                         << "regions:" << instr.regions.size();
              if(!instr.regions.empty())
              {
                auto& r = instr.regions[0];
                qWarning() << "  first region: key" << r.keyLow << "-" << r.keyHigh
                           << "sample channels:" << r.sample.data.size()
                           << "frames:"
                           << (r.sample.data.empty() ? 0 : (int)r.sample.data[0].size());
              }

              self->m_gigInfo = std::move(loaded);
              self->fileChanged();
            },
            Qt::QueuedConnection);
      });
}

}
