// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/job_service/protocol.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include <functional>

class QProcess;

namespace video_editor::app {

[[nodiscard]] QString resolveWorkerHostPath(const QString& application_directory,
                                            const QString& configured_path = {});

// Thin alias for existing callers and tests.
[[nodiscard]] QString resolveTranscriptionWorkerPath(const QString& application_directory,
                                                     const QString& configured_path);

// One fresh video_editor_worker_host process, one framed StartJob, then framed
// WorkerEvents until the process exits. kill() is the cancellation/crash
// boundary. The QProcess is a QObject child and is never leaked.
class WorkerHostSession final : public QObject {
public:
  using EventHandler = std::function<void(const jobs::v1::WorkerEvent&)>;
  using FinishedHandler = std::function<void(bool abnormal, int exit_code,
                                             QProcess::ExitStatus exit_status)>;
  using FailedHandler = std::function<void(const QString& message)>;

  struct LaunchOptions {
    QString application_directory;
    QString configured_path;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  };

  explicit WorkerHostSession(QObject* parent = nullptr);
  ~WorkerHostSession() override;

  WorkerHostSession(const WorkerHostSession&) = delete;
  WorkerHostSession& operator=(const WorkerHostSession&) = delete;

  void setEventHandler(EventHandler handler);
  void setFinishedHandler(FinishedHandler handler);
  void setStartFailedHandler(FailedHandler handler);

  // Encodes one StartJob and launches a new host. Returns false if the request
  // cannot be encoded or a process is already running. Launch failures are
  // reported asynchronously through the start-failed and finished handlers.
  [[nodiscard]] bool start(const jobs::v1::JobSpec& spec, const LaunchOptions& options);
  void cancel();
  void waitUntilFinished(int timeout_ms = 1'000);
  [[nodiscard]] bool isRunning() const noexcept;

private:
  void onStarted();
  void onReadyRead();
  void onFinished(int exit_code, QProcess::ExitStatus exit_status);
  void onErrorOccurred(QProcess::ProcessError error);
  void emitFinished(bool abnormal, int exit_code, QProcess::ExitStatus exit_status);
  void resetProcess();

  QProcess* process_{nullptr};
  QByteArray request_frame_;
  QByteArray output_buffer_;
  QString expected_job_id_;
  EventHandler event_handler_;
  FinishedHandler finished_handler_;
  FailedHandler start_failed_handler_;
  bool finished_emitted_{false};
  bool start_failed_{false};
};

} // namespace video_editor::app
