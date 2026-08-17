// SPDX-License-Identifier: MPL-2.0
#include "worker_host_session.hpp"

#include "video_editor/job_service/framing.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

#include <cstdint>
#include <string>
#include <utility>

namespace video_editor::app {

QString resolveWorkerHostPath(const QString& application_directory,
                              const QString& configured_path) {
  if (!configured_path.trimmed().isEmpty()) {
    return configured_path;
  }
#ifdef Q_OS_WIN
  constexpr auto executable = "video_editor_worker_host.exe";
#else
  constexpr auto executable = "video_editor_worker_host";
#endif
  const QDir app_dir(application_directory);
  const QString same_directory = app_dir.filePath(QString::fromUtf8(executable));
  if (QFileInfo(same_directory).isExecutable()) {
    return QDir::cleanPath(QFileInfo(same_directory).absoluteFilePath());
  }
  const QStringList candidates{
      app_dir.filePath(QStringLiteral("../workers/%1").arg(QString::fromUtf8(executable))),
      app_dir.filePath(QStringLiteral("../../src/workers/%1").arg(QString::fromUtf8(executable))),
  };
  for (const QString& candidate : candidates) {
    const QString cleaned = QDir::cleanPath(candidate);
    if (QFileInfo(cleaned).isExecutable()) {
      return QDir::cleanPath(QFileInfo(cleaned).absoluteFilePath());
    }
  }
  return QStandardPaths::findExecutable(QString::fromUtf8(executable));
}

QString resolveTranscriptionWorkerPath(const QString& application_directory,
                                       const QString& configured_path) {
  return resolveWorkerHostPath(application_directory, configured_path);
}

namespace {

[[nodiscard]] bool encode_start_request(const jobs::v1::JobSpec& spec, QByteArray& frame) {
  jobs::v1::WorkerRequest request;
  request.set_protocol_major(jobs::kProtocolMajor);
  request.set_protocol_minor(jobs::kProtocolMinor);
  *request.mutable_start()->mutable_spec() = spec;
  std::string encoded;
  if (!request.SerializeToString(&encoded) || encoded.size() > jobs::kMaximumFrameBytes) {
    return false;
  }
  frame.resize(4);
  const auto size = static_cast<std::uint32_t>(encoded.size());
  for (int index = 0; index < 4; ++index) {
    frame[index] = static_cast<char>((size >> (8 * index)) & 0xffU);
  }
  frame.append(encoded.data(), static_cast<qsizetype>(encoded.size()));
  return true;
}

} // namespace

WorkerHostSession::WorkerHostSession(QObject* parent) : QObject(parent) {}

WorkerHostSession::~WorkerHostSession() {
  cancel();
  waitUntilFinished();
  resetProcess();
}

void WorkerHostSession::setEventHandler(EventHandler handler) {
  event_handler_ = std::move(handler);
}

void WorkerHostSession::setFinishedHandler(FinishedHandler handler) {
  finished_handler_ = std::move(handler);
}

void WorkerHostSession::setStartFailedHandler(FailedHandler handler) {
  start_failed_handler_ = std::move(handler);
}

bool WorkerHostSession::start(const jobs::v1::JobSpec& spec, const LaunchOptions& options) {
  if (isRunning()) {
    return false;
  }
  QByteArray frame;
  if (!encode_start_request(spec, frame)) {
    return false;
  }

  resetProcess();
  request_frame_ = std::move(frame);
  output_buffer_.clear();
  expected_job_id_ = QString::fromStdString(spec.job_id());
  finished_emitted_ = false;
  start_failed_ = false;

  process_ = new QProcess(this);
  process_->setProcessEnvironment(options.environment);
  QObject::connect(process_, &QProcess::started, this, [this] { onStarted(); });
  QObject::connect(process_, &QProcess::readyReadStandardOutput, this, [this] { onReadyRead(); });
  QObject::connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                   [this](const int exit_code, const QProcess::ExitStatus exit_status) {
                     onFinished(exit_code, exit_status);
                   });
  QObject::connect(process_, &QProcess::errorOccurred, this,
                   [this](const QProcess::ProcessError error) { onErrorOccurred(error); });
  process_->start(resolveWorkerHostPath(options.application_directory, options.configured_path));
  return true;
}

void WorkerHostSession::cancel() {
  if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
    process_->kill();
  }
}

void WorkerHostSession::waitUntilFinished(const int timeout_ms) {
  if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
    process_->waitForFinished(timeout_ms);
  }
}

bool WorkerHostSession::isRunning() const noexcept {
  return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void WorkerHostSession::onStarted() {
  if (process_ == nullptr || request_frame_.isEmpty()) {
    return;
  }
  if (process_->write(request_frame_) != request_frame_.size()) {
    if (start_failed_handler_) {
      start_failed_handler_(QStringLiteral("Could not send the worker request."));
    }
    start_failed_ = true;
    process_->kill();
    return;
  }
  request_frame_.clear();
  process_->closeWriteChannel();
}

void WorkerHostSession::onReadyRead() {
  if (process_ == nullptr) {
    return;
  }
  output_buffer_.append(process_->readAllStandardOutput());
  while (output_buffer_.size() >= 4) {
    const auto byte = [this](const int index) {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(output_buffer_.at(index)));
    };
    const std::uint32_t size = byte(0) | (byte(1) << 8U) | (byte(2) << 16U) | (byte(3) << 24U);
    if (size > jobs::kMaximumFrameBytes) {
      cancel();
      return;
    }
    if (output_buffer_.size() < static_cast<qsizetype>(size) + 4) {
      return;
    }
    const QByteArray payload = output_buffer_.mid(4, static_cast<qsizetype>(size));
    output_buffer_.remove(0, static_cast<qsizetype>(size) + 4);
    jobs::v1::WorkerEvent event;
    if (!event.ParseFromArray(payload.constData(), static_cast<int>(payload.size())) ||
        !event.has_event()) {
      continue;
    }
    if (event.protocol_major() != jobs::kProtocolMajor ||
        event.protocol_minor() > jobs::kProtocolMinor) {
      cancel();
      return;
    }
    if (!expected_job_id_.isEmpty() &&
        event.event().job_id() != expected_job_id_.toStdString()) {
      continue;
    }
    if (!event_handler_) {
      continue;
    }
    if (event.event().state() == jobs::v1::JOB_STATE_RUNNING ||
        event.event().state() == jobs::v1::JOB_STATE_ACCEPTED) {
      event_handler_(event);
      continue;
    }
    const EventHandler handler = event_handler_;
    QMetaObject::invokeMethod(
        this, [handler, event]() { handler(event); }, Qt::QueuedConnection);
  }
}

void WorkerHostSession::onFinished(const int exit_code, const QProcess::ExitStatus exit_status) {
  onReadyRead();
  request_frame_.clear();
  output_buffer_.clear();
  process_ = nullptr;
  const bool abnormal = exit_status != QProcess::NormalExit || exit_code != 0;
  emitFinished(abnormal, exit_code, exit_status);
}

void WorkerHostSession::onErrorOccurred(const QProcess::ProcessError error) {
  if (process_ == nullptr) {
    return;
  }
  if (error != QProcess::FailedToStart) {
    return;
  }
  start_failed_ = true;
  process_ = nullptr;
  const FailedHandler handler = start_failed_handler_;
  if (handler) {
    QMetaObject::invokeMethod(
        this, [handler]() { handler(QStringLiteral("The worker host could not be started.")); },
        Qt::QueuedConnection);
  }
  emitFinished(true, -1, QProcess::CrashExit);
}

void WorkerHostSession::emitFinished(const bool abnormal, const int exit_code,
                                     const QProcess::ExitStatus exit_status) {
  if (finished_emitted_) {
    return;
  }
  finished_emitted_ = true;
  if (!finished_handler_) {
    return;
  }
  const FinishedHandler handler = finished_handler_;
  QMetaObject::invokeMethod(
      this,
      [handler, abnormal, exit_code, exit_status]() {
        handler(abnormal, exit_code, exit_status);
      },
      Qt::QueuedConnection);
}

void WorkerHostSession::resetProcess() {
  process_ = nullptr;
}

} // namespace video_editor::app
