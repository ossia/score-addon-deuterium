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
  wireInstrumentControl();
  if(!m_filePath.isEmpty())
    startAsyncLoad();
}

// The Instrument inlet drives which instrument of the file plays: changing
// it reloads the sample data asynchronously while every other control keeps
// its value. Called from every constructor once the inlets exist.
void ProcessModel::wireInstrumentControl()
{
  const int idx = 1 + Gig::Instrument;
  if(idx >= std::ssize(m_inlets))
    return;
  auto* ctl = qobject_cast<Process::ControlInlet*>(m_inlets[idx]);
  if(!ctl)
    return;

  if(ossia::convert<int>(ctl->value()) != m_instrument)
    ctl->setValue(m_instrument);

  connect(ctl, &Process::ControlInlet::valueChanged, this, [this](const ossia::value& v) {
    const int i = ossia::convert<int>(v);
    if(i == m_instrument)
      return;
    m_instrument = i;
    m_gigInfo.reset();
    if(!m_filePath.isEmpty())
      startAsyncLoad();
    else
      fileChanged();
  });

  const int fidx = 1 + Gig::File;
  if(fidx >= std::ssize(m_inlets))
    return;
  auto* fctl = qobject_cast<Process::ControlInlet*>(m_inlets[fidx]);
  if(!fctl)
    return;

  if(ossia::convert<std::string>(fctl->value()) != m_filePath.toStdString())
    fctl->setValue(m_filePath.toStdString());

  connect(
      fctl, &Process::ControlInlet::valueChanged, this, [this](const ossia::value& v) {
    const auto path = QString::fromStdString(ossia::convert<std::string>(v));
    if(path == m_filePath)
      return;
    // Keep the instrument index: swapping between similar banks keeps the
    // whole setup; out-of-range indices play silently until changed.
    loadFile(path, m_instrument);
  });
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

  // Keep the Instrument and File inlets in sync (their change handlers
  // no-op when the value already matches)
  if(const int idx = 1 + Gig::Instrument; idx < std::ssize(m_inlets))
    if(auto* ctl = qobject_cast<Process::ControlInlet*>(m_inlets[idx]))
      if(ossia::convert<int>(ctl->value()) != instrument)
        ctl->setValue(instrument);
  if(const int idx = 1 + Gig::File; idx < std::ssize(m_inlets))
    if(auto* ctl = qobject_cast<Process::ControlInlet*>(m_inlets[idx]))
      if(ossia::convert<std::string>(ctl->value()) != path.toStdString())
        ctl->setValue(path.toStdString());

  if(!path.isEmpty())
  {
    // Both phases run on a worker; fileChanged() fires when they finish
    startAsyncLoad();
  }
  else
  {
    fileChanged();
  }
}

void ProcessModel::startAsyncLoad()
{
  auto rate = score::AppContext().settings<Audio::Settings::Model>().getRate();

  // Create a new cancellation token for this load
  auto cancelToken = std::make_shared<std::atomic<bool>>(false);
  m_cancelToken = cancelToken;

  const QString path = m_filePath;
  const int instrument = m_instrument;

  // Use a QPointer so the callback is safe if ProcessModel is deleted
  QPointer<ProcessModel> self = this;

  score::TaskPool::instance().post(
      [path, instrument, rate, cancelToken, self]() mutable {
        // Phase 1: metadata parse - kept off the GUI thread, large banks can
        // take a noticeable while to walk
        auto metadata = loadGigFileMetadata(path, instrument);

        // Phase 2: load all sample data (slow)
        std::shared_ptr<GigFileInfo> loaded;
        if(metadata && !cancelToken->load(std::memory_order_relaxed))
          loaded = loadGigFileSamples(metadata, rate, cancelToken);

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
