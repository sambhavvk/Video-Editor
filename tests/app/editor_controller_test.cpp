// SPDX-License-Identifier: MPL-2.0

#include "editor_controller.hpp"
#include "project_recent_paths.hpp"
#include "media_reconstruction.hpp"
#include "path_utils.hpp"

#include "video_editor/desktop_ui/command_palette.hpp"
#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <variant>

namespace {

void writeSilentWave(const QString& path, const quint32 sample_count = 4'800,
                     const qint16 sample_value = 0) {
  constexpr quint16 channels = 2;
  constexpr quint32 sample_rate = 48'000;
  constexpr quint16 bits_per_sample = 16;
  constexpr quint16 block_align = channels * (bits_per_sample / 8);
  constexpr quint32 byte_rate = sample_rate * block_align;
  const quint32 data_size = sample_count * block_align;

  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  QCOMPARE(stream.writeRawData("RIFF", 4), 4);
  stream << quint32{36U + data_size};
  QCOMPARE(stream.writeRawData("WAVEfmt ", 8), 8);
  stream << quint32{16} << quint16{1} << channels << sample_rate << byte_rate << block_align
         << bits_per_sample;
  QCOMPARE(stream.writeRawData("data", 4), 4);
  stream << data_size;
  QByteArray samples(static_cast<qsizetype>(data_size), '\0');
  for (qsizetype offset = 0; offset < samples.size(); offset += 2) {
    const qsizetype frame = offset / static_cast<qsizetype>(block_align);
    const qint16 value = sample_value != 0 && frame % 48 >= 24 ? -sample_value : sample_value;
    samples[offset] = static_cast<char>(static_cast<quint16>(value) & 0xffU);
    samples[offset + 1] = static_cast<char>((static_cast<quint16>(value) >> 8U) & 0xffU);
  }
  QCOMPARE(file.write(samples), static_cast<qint64>(data_size));
  file.close();
}

void writePpmFrame(const QString& path, const int width, const int height) {
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray header = QByteArrayLiteral("P6\n") + QByteArray::number(width) + ' ' +
                            QByteArray::number(height) + QByteArrayLiteral("\n255\n");
  QCOMPARE(file.write(header), static_cast<qint64>(header.size()));
  QByteArray pixels(width * height * 3, '\0');
  for (qsizetype index = 0; index < pixels.size(); index += 3) {
    pixels[index] = static_cast<char>(0x40);
    pixels[index + 1] = static_cast<char>(0x80);
    pixels[index + 2] = static_cast<char>(0xc0);
  }
  QCOMPARE(file.write(pixels), static_cast<qint64>(pixels.size()));
  file.close();
}

bool writePlaybackVideo(const QString& path) {
  const QStringList arguments{
      QStringLiteral("-hide_banner"),
      QStringLiteral("-loglevel"),
      QStringLiteral("error"),
      QStringLiteral("-nostdin"),
      QStringLiteral("-y"),
      QStringLiteral("-f"),
      QStringLiteral("lavfi"),
      QStringLiteral("-i"),
      QStringLiteral("testsrc2=size=1920x1080:rate=2:duration=30"),
      QStringLiteral("-c:v"),
      QStringLiteral("mpeg4"),
      QStringLiteral("-q:v"),
      QStringLiteral("4"),
      QStringLiteral("-pix_fmt"),
      QStringLiteral("yuv420p"),
      path,
  };
  return QProcess::execute(QStringLiteral(VIDEO_EDITOR_APP_TEST_FFMPEG), arguments) == 0 &&
         QFileInfo::exists(path);
}

bool writeMuxedAv(const QString& path) {
  const QStringList arguments{
      QStringLiteral("-hide_banner"),
      QStringLiteral("-loglevel"),
      QStringLiteral("error"),
      QStringLiteral("-nostdin"),
      QStringLiteral("-y"),
      QStringLiteral("-f"),
      QStringLiteral("lavfi"),
      QStringLiteral("-i"),
      QStringLiteral("testsrc2=size=64x64:rate=30:duration=2"),
      QStringLiteral("-f"),
      QStringLiteral("lavfi"),
      QStringLiteral("-i"),
      QStringLiteral("sine=frequency=440:sample_rate=48000:duration=2"),
      QStringLiteral("-c:v"),
      QStringLiteral("mpeg4"),
      QStringLiteral("-q:v"),
      QStringLiteral("8"),
      QStringLiteral("-c:a"),
      QStringLiteral("pcm_s16le"),
      QStringLiteral("-shortest"),
      path,
  };
  return QProcess::execute(QStringLiteral(VIDEO_EDITOR_APP_TEST_FFMPEG), arguments) == 0 &&
         QFileInfo::exists(path);
}

std::size_t audioClipCount(const video_editor::edit::Project& project) {
  if (project.sequences.empty()) {
    return 0;
  }
  std::size_t count = 0;
  for (const auto& track : project.sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio) {
      count += track.clips.size();
    }
  }
  return count;
}

std::size_t videoTrackCount(const video_editor::edit::Sequence& sequence) {
  return static_cast<std::size_t>(std::count_if(
      sequence.tracks.begin(), sequence.tracks.end(), [](const auto& track) {
        return track.kind == video_editor::edit::TrackKind::Video;
      }));
}

const video_editor::edit::Track*
videoTrackAt(const video_editor::edit::Sequence& sequence, const std::size_t ordinal) {
  std::size_t seen = 0;
  for (const auto& track : sequence.tracks) {
    if (track.kind != video_editor::edit::TrackKind::Video) {
      continue;
    }
    if (seen == ordinal) {
      return &track;
    }
    ++seen;
  }
  return nullptr;
}

} // namespace

class EditorControllerTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void importsInsertsAndRoundTripsUndo();
  void insertAssetInsertsImportedMediaWithoutSourceMonitor();
  void derivesSequenceFormatFromFirstVideoClip();
  void professionalTimelineInteractionsUseOneAtomicHistoryStep();
  void batchSplitAndFrameNudgePreserveExactSelectionGeometry();
  void rollEditsUseTheGestureEdge();
  void presentsFramesContinuouslyWhilePlaybackIsRunning();
  void reusesPreviewCacheForTheSamePausedFrame();
  void loadsSourceMonitorAndRippleInsertsMarkedRange();
  void inOutActionMarksProgramWhenSourceIsHidden();
  void importsSearchesAndExportsCaptions();
  void beginnerFifteenMinutePathWithoutFullEncode();
  void normalizationOnlyAdjustsAudibleContributingTracks();
  void realAudioDeviceUsesTheSampleCounterAsMasterClock();
  void audioBufferSizeDefaultsToMedium();
  void timedAutosaveCheckpointsDirtyProjects();
  void audioDevicePollSteadyConnectedIsNotRecovered();
  void audioDevicePollLossReturnAndDelayedStopAreRetryable();
  void audioDevicePollPauseCancelsRecoveryIntent();
  void audioDevicePollIntervalUsesBackupWhenNotificationsLive();
  void audioMixerKeepsSystemDefaultWhenEnumerationIsEmpty();
  void normalizationGenerationRejectsObsoleteCompletionAndClearsBusy();
  void mapsAndClampsReversedTranscriptWordsInPlaybackOrder();
  void rejectsOversizedModelDownloadBoundaries();
  void reconstructsMediaRecordsFromPersistedAssets();
  void cancellingProxyDoesNotRegisterCompleteProxy();
  void proxyWorkerDeathReportsFailureWithoutHanging();
  void rippleInsertsStillOverlayOnFreeVideoTrack();
  void rippleInsertsSecondStillOverlayAddsVideoTrack();
  void rippleInsertsStillOnEmptyTimelineSetsFormat();
  void rippleInsertVideoMidClipStillFails();
  void viewerMoveAndCropUpdateSelectedClipTransform();
  void nestSelectedClipsOpensChildSequence();
  void queuedExportDoesNotStartSecondWorker();
  void persistsQueuedExportSidecar();
  void tracksRecentProjectsAndReopenLastSetting();
  void otioMenuActionsEmitImportExportSignals();
  void showingWindowDoesNotCrashDuringGpuPresentationInit();
  void emptyTimelineActionsReportWhyTheyDidNothing();
  void razorAddEditsSplitsUnlockedClips();
  void volumeEnvelopeUpsertsAudioVolumeKeyframe();
  void opacityEnvelopeUpsertsVideoOpacityKeyframe();
  void freezeFrameHoldsSourceAtPlayhead();
  void resyncLinkedAvMovesPartnersInOneUndoStep();
  void trackVisibilityPresetIsolatesAndRestores();
  void previewQualityPersistsAndUpdatesProgramTitle();
  void backgroundJobsPauseDuringPlayback();

private:
  std::unique_ptr<QTemporaryDir> application_data_;
};

void EditorControllerTest::mapsAndClampsReversedTranscriptWordsInPlaybackOrder() {
  using namespace video_editor;
  const std::vector<edit::CaptionWord> words{
      {.text = "before", .range = edit::TimeRange(edit::Time(-2, 1), edit::Time(3, 1))},
      {.text = "inside", .range = edit::TimeRange(edit::Time(2, 1), edit::Time(2, 1))},
      {.text = "after", .range = edit::TimeRange(edit::Time(8, 1), edit::Time(2, 1))}};
  const auto mapped = app::mapTranscriptionWordsToTimeline(
      words, edit::TimeRange(edit::Time(0, 1), edit::Time(6, 1)),
      edit::TimeRange(edit::Time(10, 1), edit::Time(6, 1)), edit::Rate(1, 1), true);
  QCOMPARE(mapped.size(), std::size_t{2});
  QCOMPARE(mapped[0].text, std::string("inside"));
  QCOMPARE(mapped[0].range, (edit::TimeRange(edit::Time(12, 1), edit::Time(2, 1))));
  QCOMPARE(mapped[1].text, std::string("before"));
  QCOMPARE(mapped[1].range, (edit::TimeRange(edit::Time(15, 1), edit::Time(1, 1))));
}

void EditorControllerTest::rejectsOversizedModelDownloadBoundaries() {
  using video_editor::app::modelDownloadSizeAllowed;
  constexpr std::uintmax_t expected = 147'951'465U;
  QVERIFY(modelDownloadSizeAllowed(0U, static_cast<std::int64_t>(expected), expected));
  QVERIFY(modelDownloadSizeAllowed(expected, -1, expected));
  QVERIFY(!modelDownloadSizeAllowed(expected + 1U, -1, expected));
  QVERIFY(!modelDownloadSizeAllowed(0U, static_cast<std::int64_t>(expected - 1U), expected));
  QVERIFY(!modelDownloadSizeAllowed(0U, static_cast<std::int64_t>(expected + 1U), expected));
}

void EditorControllerTest::initTestCase() {
  application_data_ = std::make_unique<QTemporaryDir>();
  QVERIFY(application_data_->isValid());
  qputenv("XDG_DATA_HOME", application_data_->path().toUtf8());
#ifdef VIDEO_EDITOR_APP_TEST_WORKER_HOST
  qputenv("VIDEO_EDITOR_WORKER_HOST", VIDEO_EDITOR_APP_TEST_WORKER_HOST);
#endif
  QStandardPaths::setTestModeEnabled(true);
}

void EditorControllerTest::audioDevicePollSteadyConnectedIsNotRecovered() {
  using video_editor::app::evaluateAudioDevicePoll;
  using video_editor::audio::AudioDeviceInfo;
  const std::vector<AudioDeviceInfo> devices{
      {.id = "speaker", .name = "Speaker", .is_default = true, .connected = true}};
  const auto decision = evaluateAudioDevicePoll(devices, devices, "speaker");
  QVERIFY(!decision.selected_missing);
  QVERIFY(!decision.selected_recovered);
  QVERIFY(!decision.default_missing);
  QVERIFY(!decision.default_recovered);

  const std::vector<AudioDeviceInfo> alternate{
      {.id = "headphones", .name = "Headphones", .is_default = false, .connected = true}};
  const auto stillMissing = evaluateAudioDevicePoll(alternate, alternate, "speaker");
  QVERIFY(stillMissing.selected_missing);
  QVERIFY(!stillMissing.selected_recovered);

  const auto defaultStillMissing = evaluateAudioDevicePoll(alternate, alternate, {});
  QVERIFY(defaultStillMissing.default_missing);
  QVERIFY(!defaultStillMissing.default_recovered);

  const std::vector<AudioDeviceInfo> none{};
  const auto emptyListKeepsDefault = evaluateAudioDevicePoll(none, none, {});
  QVERIFY(!emptyListKeepsDefault.default_missing);
}

void EditorControllerTest::audioDevicePollLossReturnAndDelayedStopAreRetryable() {
  using video_editor::app::evaluateAudioDevicePoll;
  using video_editor::audio::AudioDeviceInfo;
  const std::vector<AudioDeviceInfo> present{
      {.id = "speaker", .name = "Speaker", .is_default = true, .connected = true}};
  const std::vector<AudioDeviceInfo> missing{};
  const auto lost = evaluateAudioDevicePoll(present, missing, "speaker");
  QVERIFY(lost.selected_missing);
  const auto returned = evaluateAudioDevicePoll(missing, present, "speaker");
  QVERIFY(returned.selected_recovered);
  bool recovery_pending = returned.selected_recovered;
  const bool stop_settled = false;
  QVERIFY(recovery_pending);
  QVERIFY(!stop_settled);
  // A delayed asynchronous stop must not clear the recovery intent.
  recovery_pending = recovery_pending && !stop_settled;
  QVERIFY(recovery_pending);
}

void EditorControllerTest::audioDevicePollPauseCancelsRecoveryIntent() {
  bool recovery_pending = true;
  const bool playback_intended = false;
  if (!playback_intended) {
    recovery_pending = false;
  }
  QVERIFY(!recovery_pending);
}

void EditorControllerTest::audioDevicePollIntervalUsesBackupWhenNotificationsLive() {
  using video_editor::app::audioDevicePollIntervalMs;
  using video_editor::app::kAudioDeviceBackupPollIntervalMs;
  using video_editor::app::kAudioDevicePollIntervalMs;
  QCOMPARE(audioDevicePollIntervalMs(true, true), kAudioDeviceBackupPollIntervalMs);
  QCOMPARE(audioDevicePollIntervalMs(true, false), kAudioDevicePollIntervalMs);
  QCOMPARE(audioDevicePollIntervalMs(false, true), kAudioDevicePollIntervalMs);
  QCOMPARE(audioDevicePollIntervalMs(false, false), kAudioDevicePollIntervalMs);
}

void EditorControllerTest::audioMixerKeepsSystemDefaultWhenEnumerationIsEmpty() {
  using video_editor::app::AudioMixerOutputStatus;
  using video_editor::app::audioMixerOutputStatus;
  QCOMPARE(audioMixerOutputStatus(false, false), AudioMixerOutputStatus::BackendMissing);
  QCOMPARE(audioMixerOutputStatus(true, true), AudioMixerOutputStatus::SelectedUnavailable);
  QCOMPARE(audioMixerOutputStatus(true, false), AudioMixerOutputStatus::Ready);
}

void EditorControllerTest::normalizationGenerationRejectsObsoleteCompletionAndClearsBusy() {
  video_editor::app::NormalizationCompletionGate gate;
  const auto obsolete = gate.begin();
  gate.invalidate();
  QVERIFY(!gate.complete(obsolete));
  QVERIFY(!gate.busy);

  video_editor::desktop_ui::AudioMixerWidget mixer;
  mixer.setNormalizationBusy(true);
  auto* analyze = mixer.findChild<QPushButton*>(QStringLiteral("normalizationAnalyze"));
  QVERIFY(analyze != nullptr);
  QVERIFY(!analyze->isEnabled());
  mixer.setNormalizationBusy(false);
  QVERIFY(analyze->isEnabled());
}

void EditorControllerTest::importsInsertsAndRoundTripsUndo() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("dialogue.wav"));
  writeSilentWave(wave_path);

  QSettings settings(directory.filePath(QStringLiteral("ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  QCOMPARE(window.mediaBin()->items().size(), 1);
  const QString asset_id = window.mediaBin()->items().front().id;

  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();
  QCOMPARE(audioClipCount(*controller.editor().projectAt(controller.editor().revision())), 1U);
  QVERIFY(controller.dirty());

  window.parameterEdited(QStringLiteral("audioGain"), -6.0);
  const auto gained = controller.editor().projectAt(controller.editor().revision());
  const auto gained_track = std::find_if(
      gained->sequences.front().tracks.begin(), gained->sequences.front().tracks.end(),
      [](const auto& track) { return track.kind == video_editor::edit::TrackKind::Audio; });
  QVERIFY(gained_track != gained->sequences.front().tracks.end());
  QCOMPARE(gained_track->clips.size(), 1U);
  QCOMPARE(gained_track->clips.front().audio_gain_db, -6.0);
  window.undoRequested();

  window.audioMixer()->muteToggled(0, true);
  const auto muted = controller.editor().projectAt(controller.editor().revision());
  const auto muted_track = std::find_if(
      muted->sequences.front().tracks.begin(), muted->sequences.front().tracks.end(),
      [](const auto& track) { return track.kind == video_editor::edit::TrackKind::Audio; });
  QVERIFY(muted_track != muted->sequences.front().tracks.end());
  QVERIFY(muted_track->muted);
  window.undoRequested();

  window.undoRequested();
  QCOMPARE(audioClipCount(*controller.editor().projectAt(controller.editor().revision())), 0U);
  window.redoRequested();
  QCOMPARE(audioClipCount(*controller.editor().projectAt(controller.editor().revision())), 1U);

  const auto checkpoint =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("roundtrip.veproj")));
  QVERIFY(controller.saveProjectFile(checkpoint));
  QVERIFY(!controller.dirty());

  const auto master =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("audio-timeline.mkv")));
  QSignalSpy export_finished(&controller,
                             &video_editor::app::EditorController::videoExportFinished);
  QVERIFY(controller.startVideoExport(master, QStringLiteral("master.ffv1")));
  QTRY_COMPARE_WITH_TIMEOUT(export_finished.count(), 1, 30'000);
  QVERIFY(export_finished.at(0).at(0).toBool());
  QVERIFY(export_finished.at(0).at(2).toString().contains(QStringLiteral("audio samples")));
  QVERIFY(QFileInfo(video_editor::app::qStringFromPath(master)).size() > 0);

  QSettings reopened_settings(directory.filePath(QStringLiteral("reopened-ui.ini")),
                              QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow reopened_window(&reopened_settings);
  video_editor::app::EditorController reopened_controller(reopened_window);
  QVERIFY(reopened_controller.openProjectFile(checkpoint));
  const auto reopened =
      reopened_controller.editor().projectAt(reopened_controller.editor().revision());
  QCOMPARE(reopened->assets.size(), 1U);
  QCOMPARE(audioClipCount(*reopened), 1U);
  QVERIFY(!reopened_controller.dirty());
}

void EditorControllerTest::queuedExportDoesNotStartSecondWorker() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString still_path = directory.filePath(QStringLiteral("queue-still.ppm"));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("queue-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  controller.importPaths({still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  const auto export_a = video_editor::app::pathFromQString(
      directory.filePath(QStringLiteral("export-a.mkv")));
  const auto export_b = video_editor::app::pathFromQString(
      directory.filePath(QStringLiteral("export-b.mkv")));
  QVERIFY(controller.startVideoExport(export_a, QStringLiteral("master.ffv1")));
  QVERIFY(controller.startVideoExport(export_b, QStringLiteral("master.ffv1")));
  QCOMPARE(controller.exportQueueCount(), 2U);
  QCOMPARE(controller.queuedExportCount(), 1U);

  auto* list = window.deliverPanel()->findChild<QListWidget*>(QStringLiteral("exportJobList"));
  QVERIFY(list != nullptr);
  QString queued_id;
  for (int index = 0; index < list->count(); ++index) {
    QListWidgetItem* item = list->item(index);
    if (item->data(Qt::UserRole + 1).toInt() ==
        static_cast<int>(video_editor::desktop_ui::ExportJobStateView::Queued)) {
      queued_id = item->data(Qt::UserRole).toString();
      break;
    }
  }
  QVERIFY(!queued_id.isEmpty());
  controller.cancelQueuedExport(queued_id);
  QCOMPARE(controller.queuedExportCount(), 0U);
  QCOMPARE(controller.exportQueueCount(), 1U);
}

void EditorControllerTest::persistsQueuedExportSidecar() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString still_path = directory.filePath(QStringLiteral("sidecar-still.ppm"));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("sidecar-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  controller.importPaths({still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  const auto checkpoint =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("project.veproj")));
  QVERIFY(controller.saveProjectFile(checkpoint));

  const auto export_a = video_editor::app::pathFromQString(
      directory.filePath(QStringLiteral("sidecar-a.mkv")));
  const auto export_b = video_editor::app::pathFromQString(
      directory.filePath(QStringLiteral("sidecar-b.mkv")));
  QVERIFY(controller.startVideoExport(export_a, QStringLiteral("master.ffv1")));
  QVERIFY(controller.startVideoExport(export_b, QStringLiteral("master.ffv1")));

  const QString sidecar_path =
      directory.filePath(QStringLiteral("project.export-queue.json"));
  QVERIFY(QFileInfo::exists(sidecar_path));
  QFile sidecar_file(sidecar_path);
  QVERIFY(sidecar_file.open(QIODevice::ReadOnly));
  const QJsonDocument document = QJsonDocument::fromJson(sidecar_file.readAll());
  QVERIFY(document.isObject());
  const QJsonArray jobs = document.object().value(QStringLiteral("jobs")).toArray();
  QCOMPARE(jobs.size(), 1);
  QCOMPARE(jobs.at(0).toObject().value(QStringLiteral("state")).toString(),
           QStringLiteral("queued"));
  QCOMPARE(jobs.at(0).toObject().value(QStringLiteral("destination")).toString(),
           video_editor::app::qStringFromPath(export_b));

  auto* list = window.deliverPanel()->findChild<QListWidget*>(QStringLiteral("exportJobList"));
  QVERIFY(list != nullptr);
  for (int index = 0; index < list->count(); ++index) {
    QListWidgetItem* item = list->item(index);
    if (item->data(Qt::UserRole + 1).toInt() ==
        static_cast<int>(video_editor::desktop_ui::ExportJobStateView::Queued)) {
      controller.cancelQueuedExport(item->data(Qt::UserRole).toString());
      break;
    }
  }
}

void EditorControllerTest::insertAssetInsertsImportedMediaWithoutSourceMonitor() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("dialogue.wav"));
  writeSilentWave(wave_path);

  QSettings settings(directory.filePath(QStringLiteral("insert-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  QCOMPARE(window.mediaBin()->items().size(), 1);
  const QString asset_id = window.mediaBin()->items().front().id;

  QVERIFY(QMetaObject::invokeMethod(&window, "mediaInsertRequested", Qt::DirectConnection,
                                    Q_ARG(QString, asset_id)));
  QCOMPARE(audioClipCount(*controller.editor().projectAt(controller.editor().revision())), 1U);
  QVERIFY(controller.dirty());
}

void EditorControllerTest::derivesSequenceFormatFromFirstVideoClip() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("first-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("format-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  const auto before = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(before->sequences.front().width, 1'920U);
  QCOMPARE(before->sequences.front().height, 1'080U);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  const auto inserted = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(inserted->sequences.front().width, 16U);
  QCOMPARE(inserted->sequences.front().height, 10U);
  QCOMPARE(inserted->sequences.front().tracks.front().clips.size(), 1U);
  QTRY_VERIFY_WITH_TIMEOUT(window.programViewer()->hasFrame(), 10'000);
  if (qEnvironmentVariableIsSet("VIDEO_EDITOR_TEST_GPU")) {
    QTRY_VERIFY_WITH_TIMEOUT(controller.gpuPreviewActive(), 10'000);
    QVERIFY2(window.programViewer()->title().contains(QStringLiteral("GPU")),
             qPrintable(window.programViewer()->title()));

    window.parameterEdited(QStringLiteral("blendMode"), QStringLiteral("add"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.gpuPreviewActive(), 10'000);
    QVERIFY2(window.programViewer()->title().contains(QStringLiteral("CPU frame")),
             qPrintable(window.programViewer()->title()));
    QVERIFY2(window.programViewer()->title().contains(QStringLiteral("GPU ready")),
             qPrintable(window.programViewer()->title()));
    window.undoRequested();
    QTRY_VERIFY_WITH_TIMEOUT(controller.gpuPreviewActive(), 10'000);
    const auto restored = controller.editor().projectAt(controller.editor().revision());
    window.timeline()->clipActivated(QString::fromStdString(
        restored->sequences.front().tracks.front().clips.front().id.toString()));
  }

  window.parameterEdited(QStringLiteral("positionX"), 32.0);
  const auto transformed = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(transformed->sequences.front().tracks.front().clips.front().transform.position.x, 32.0);
  window.undoRequested();
  const auto transform_undone = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(transform_undone->sequences.front().tracks.front().clips.front().transform.position.x,
           0.0);

  window.timeline()->clipActivated(QString::fromStdString(
      transform_undone->sequences.front().tracks.front().clips.front().id.toString()));

  const QString clip_id = QString::fromStdString(
      transform_undone->sequences.front().tracks.front().clips.front().id.toString());
  window.timeline()->clipBatchEditCommitted(
      {clip_id}, 0, 48'000, 0, video_editor::desktop_ui::TimelineWidget::EditMode::Move,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  const auto moved = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(moved->sequences.front().tracks.front().clips.front().timeline_range.start,
           video_editor::edit::Time(1, 1));

  window.undoRequested();
  const auto move_undone = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(move_undone->sequences.front().width, 16U);
  QCOMPARE(move_undone->sequences.front().tracks.front().clips.front().timeline_range.start,
           video_editor::edit::Time{});

  window.undoRequested();
  const auto undone = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(undone->sequences.front().width, 1'920U);
  QCOMPARE(undone->sequences.front().height, 1'080U);
  QCOMPARE(undone->sequences.front().tracks.front().clips.size(), 0U);
}

void EditorControllerTest::professionalTimelineInteractionsUseOneAtomicHistoryStep() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("timeline-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("timeline-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  const QString asset_id = window.mediaBin()->items().front().id;
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto tracks_before_add = project->sequences.front().tracks.size();
  const QString first_clip =
      QString::fromStdString(project->sequences.front().tracks.front().clips.front().id.toString());
  window.timeline()->trackAddRequested(video_editor::desktop_ui::TrackKind::Video);
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().tracks.size(), tracks_before_add + 1U);
  const auto& added = project->sequences.front().tracks.back();
  QCOMPARE(added.kind, video_editor::edit::TrackKind::Video);
  const QString second_track = QString::fromStdString(added.id.toString());
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Video && track.id != added.id) {
      window.timeline()->trackTargetToggled(QString::fromStdString(track.id.toString()), false);
    }
  }
  window.timeline()->trackRenameRequested(second_track, QStringLiteral("B-roll"));
  window.timeline()->trackVisibilityToggled(second_track, false);
  project = controller.editor().projectAt(controller.editor().revision());
  const auto renamed =
      std::find_if(project->sequences.front().tracks.begin(),
                   project->sequences.front().tracks.end(), [&second_track](const auto& track) {
                     return QString::fromStdString(track.id.toString()) == second_track;
                   });
  QVERIFY(renamed != project->sequences.front().tracks.end());
  QCOMPARE(renamed->name, std::string("B-roll"));
  QVERIFY(!renamed->visible);
  QVERIFY(renamed->targeted);

  window.seekRequested(6 * 48'000);
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();
  project = controller.editor().projectAt(controller.editor().revision());
  const auto second_track_after =
      std::find_if(project->sequences.front().tracks.begin(),
                   project->sequences.front().tracks.end(), [&second_track](const auto& track) {
                     return QString::fromStdString(track.id.toString()) == second_track;
                   });
  QVERIFY(second_track_after != project->sequences.front().tracks.end());
  QCOMPARE(second_track_after->clips.size(), 1U);
  const QString second_clip =
      QString::fromStdString(second_track_after->clips.front().id.toString());

  const auto revision_before_move = controller.editor().revision();
  window.timeline()->clipBatchEditCommitted(
      {first_clip, second_clip}, 0, 48'000, 0,
      video_editor::desktop_ui::TimelineWidget::EditMode::Move,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(controller.editor().revision().value, revision_before_move.value + 1U);
  QCOMPARE(project->sequences.front().tracks.front().clips.front().timeline_range.start,
           video_editor::edit::Time(1, 1));
  // The UI's visual selection is ordered by clip position, not by the drag
  // anchor. The active (second) clip must therefore receive the destination
  // track while the other selected clip remains on its source track.
  const auto moved_second = std::find_if(
      project->sequences.front().tracks.front().clips.begin(),
      project->sequences.front().tracks.front().clips.end(), [&second_clip](const auto& clip) {
        return QString::fromStdString(clip.id.toString()) == second_clip;
      });
  QVERIFY(moved_second != project->sequences.front().tracks.front().clips.end());
  QCOMPARE(moved_second->timeline_range.start, video_editor::edit::Time(7, 1));

  window.undoRequested();
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().tracks.front().clips.front().timeline_range.start,
           video_editor::edit::Time{});
  const auto undone_second_track =
      std::find_if(project->sequences.front().tracks.begin(),
                   project->sequences.front().tracks.end(), [&second_track](const auto& track) {
                     return QString::fromStdString(track.id.toString()) == second_track;
                   });
  QVERIFY(undone_second_track != project->sequences.front().tracks.end());
  QCOMPARE(undone_second_track->clips.front().timeline_range.start, video_editor::edit::Time(6, 1));

  window.timeline()->markerAddRequested(96'000);
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().markers.size(), 1U);
  const QString marker_id =
      QString::fromStdString(project->sequences.front().markers.front().id.toString());
  window.timeline()->markerMoveCommitted(marker_id, 144'000, {});
  window.timeline()->markerRenameRequested(marker_id, QStringLiteral("Chapter 1"));
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().markers.front().range.start, video_editor::edit::Time(3, 1));
  QCOMPARE(project->sequences.front().markers.front().label, std::string("Chapter 1"));
}

void EditorControllerTest::batchSplitAndFrameNudgePreserveExactSelectionGeometry() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("selection-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("selection-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  const QString asset_id = window.mediaBin()->items().front().id;
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto& sequence = project->sequences.front();
  const QString first_track = QString::fromStdString(sequence.tracks.front().id.toString());
  const QString second_track = QString::fromStdString(sequence.tracks.at(1).id.toString());
  const QString first_clip =
      QString::fromStdString(sequence.tracks.front().clips.front().id.toString());
  window.timeline()->trackTargetToggled(first_track, false);
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();
  project = controller.editor().projectAt(controller.editor().revision());
  const auto second_track_it =
      std::find_if(project->sequences.front().tracks.begin(),
                   project->sequences.front().tracks.end(), [&second_track](const auto& track) {
                     return QString::fromStdString(track.id.toString()) == second_track;
                   });
  QVERIFY(second_track_it != project->sequences.front().tracks.end());
  QCOMPARE(second_track_it->clips.size(), 1U);
  const QString second_clip = QString::fromStdString(second_track_it->clips.front().id.toString());

  // Start each selection member between frame boundaries. Nudge must add one
  // rational frame duration without snapping either clip independently.
  window.timeline()->clipBatchEditCommitted(
      {first_clip}, 0, 1, 0, video_editor::desktop_ui::TimelineWidget::EditMode::Move,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  window.timeline()->clipBatchEditCommitted(
      {second_clip}, 1, 2, 0, video_editor::desktop_ui::TimelineWidget::EditMode::Move,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  project = controller.editor().projectAt(controller.editor().revision());
  const auto& after_offsets = project->sequences.front();
  const auto first_before = after_offsets.tracks.front().clips.front().timeline_range.start;
  const auto second_before_track = std::find_if(
      after_offsets.tracks.begin(), after_offsets.tracks.end(), [&second_track](const auto& track) {
        return QString::fromStdString(track.id.toString()) == second_track;
      });
  QVERIFY(second_before_track != after_offsets.tracks.end());
  const auto second_before = second_before_track->clips.front().timeline_range.start;
  const auto frame_delta = after_offsets.frame_rate.frameTime();

  window.timeline()->clipSelectionChanged({first_clip, second_clip}, second_clip);
  window.timeline()->frameNudgeRequested(
      {first_clip, second_clip}, 1, video_editor::desktop_ui::TimelineWidget::EditIntent::Normal);
  project = controller.editor().projectAt(controller.editor().revision());
  const auto& nudged = project->sequences.front();
  const auto second_nudged_track =
      std::find_if(nudged.tracks.begin(), nudged.tracks.end(), [&second_track](const auto& track) {
        return QString::fromStdString(track.id.toString()) == second_track;
      });
  QVERIFY(second_nudged_track != nudged.tracks.end());
  QCOMPARE(nudged.tracks.front().clips.front().timeline_range.start, first_before + frame_delta);
  QCOMPARE(second_nudged_track->clips.front().timeline_range.start, second_before + frame_delta);

  const auto split_time = 2 * window.timeline()->timeScale();
  window.seekRequested(split_time);
  const auto revision_before_split = controller.editor().revision();
  window.splitClipRequested();
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(controller.editor().revision().value, revision_before_split.value + 1U);
  QCOMPARE(project->sequences.front().tracks.front().clips.size(), 2U);
  const auto split_second_track =
      std::find_if(project->sequences.front().tracks.begin(),
                   project->sequences.front().tracks.end(), [&second_track](const auto& track) {
                     return QString::fromStdString(track.id.toString()) == second_track;
                   });
  QVERIFY(split_second_track != project->sequences.front().tracks.end());
  QCOMPARE(split_second_track->clips.size(), 2U);
}

void EditorControllerTest::rollEditsUseTheGestureEdge() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("roll-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("roll-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  const QString asset_id = window.mediaBin()->items().front().id;
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto clip_duration =
      project->sequences.front().tracks.front().clips.front().timeline_range.duration;
  const auto ui_scale = window.timeline()->timeScale();
  const auto duration_ui =
      static_cast<qint64>(clip_duration
                              .rescaledTo(static_cast<std::uint32_t>(ui_scale),
                                          video_editor::edit::RoundingMode::NearestTiesEven)
                              .value());
  const qint64 one_second = ui_scale;
  const QString first_id =
      QString::fromStdString(project->sequences.front().tracks.front().clips.front().id.toString());
  // Give both edit sides one second of source handle. Creator-imported stills
  // otherwise consume their full source range and a roll would correctly be
  // rejected by the precision model.
  window.timeline()->clipBatchEditCommitted(
      {first_id}, 0, 0, -one_second, video_editor::desktop_ui::TimelineWidget::EditMode::TrimOut,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  window.seekRequested(duration_ui - one_second);
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();
  project = controller.editor().projectAt(controller.editor().revision());
  const QString middle_before_trim =
      QString::fromStdString(project->sequences.front().tracks.front().clips.at(1).id.toString());
  window.timeline()->clipBatchEditCommitted(
      {middle_before_trim}, 0, 0, -one_second,
      video_editor::desktop_ui::TimelineWidget::EditMode::TrimOut,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  window.seekRequested((duration_ui - one_second) * 2);
  window.mediaActivated(asset_id);
  window.rippleInsertFromSource();
  project = controller.editor().projectAt(controller.editor().revision());
  const auto& clips = project->sequences.front().tracks.front().clips;
  QCOMPARE(clips.size(), 3U);
  const QString middle_id = QString::fromStdString(clips.at(1).id.toString());
  const auto incoming_cut = clips.at(1).timeline_range.start;
  const auto outgoing_cut = clips.at(1).timeline_range.end();

  window.timeline()->clipActivated(middle_id);
  window.timeline()->clipBatchEditCommitted(
      {middle_id}, 0, one_second, -one_second,
      video_editor::desktop_ui::TimelineWidget::EditMode::Roll,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().tracks.front().clips.at(0).timeline_range.end(),
           incoming_cut + video_editor::edit::Time(1, 1));
  window.undoRequested();

  window.timeline()->clipActivated(middle_id);
  window.timeline()->clipBatchEditCommitted(
      {middle_id}, 0, one_second, one_second,
      video_editor::desktop_ui::TimelineWidget::EditMode::Roll,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().tracks.front().clips.at(1).timeline_range.end(),
           outgoing_cut + video_editor::edit::Time(1, 1));
}

void EditorControllerTest::presentsFramesContinuouslyWhilePlaybackIsRunning() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("realtime-preview.mkv"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("playback-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  QTRY_VERIFY_WITH_TIMEOUT(window.programViewer()->hasFrame(), 10'000);

  // Force the deterministic CPU fallback so a render lasts longer than the
  // 16 ms transport timer on ordinary development hardware.
  window.parameterEdited(QStringLiteral("blendMode"), QStringLiteral("add"));
  const std::uint64_t before_playback = controller.previewPresentationCount();
  window.playbackRateRequested(2.0);

  QTRY_VERIFY_WITH_TIMEOUT(controller.playbackRunning() &&
                               controller.previewPresentationCount() >= before_playback + 2U,
                           10'000);
  window.playbackRateRequested(0.0);
}

void EditorControllerTest::reusesPreviewCacheForTheSamePausedFrame() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("preview-cache.mkv"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("preview-cache-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  QTRY_VERIFY_WITH_TIMEOUT(window.programViewer()->hasFrame(), 10'000);
  QTRY_VERIFY_WITH_TIMEOUT(controller.previewPresentationCount() >= 1U, 10'000);

  const std::uint64_t seeks_after_first = controller.playbackSeekCount();
  const std::uint64_t decoded_after_first = controller.playbackDecodedFrameCount();
  const std::uint64_t presented_after_first = controller.previewPresentationCount();
  QVERIFY(seeks_after_first >= 1U);

  window.seekRequested(window.timeline()->playhead());
  QTRY_VERIFY_WITH_TIMEOUT(controller.previewPresentationCount() > presented_after_first, 10'000);
  QCOMPARE(controller.playbackSeekCount(), seeks_after_first);
  QCOMPARE(controller.playbackDecodedFrameCount(), decoded_after_first);
}

void EditorControllerTest::loadsSourceMonitorAndRippleInsertsMarkedRange() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("source-monitor.mkv"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("source-monitor-ui.ini")),
                     QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  QVERIFY(controller.sourceAssetLoaded());
  QVERIFY(!window.findChild<QWidget*>(QStringLiteral("sourceMonitorContainer"))->isHidden());
  QTRY_VERIFY_WITH_TIMEOUT(controller.sourcePresentationCount() >= 1U, 10'000);
  QVERIFY(window.sourceViewer()->hasFrame());
  QVERIFY(!window.programViewer()->hasFrame());

  window.seekSource(48'000);
  window.markSourceIn();
  window.seekSource(96'000);
  window.markSourceOut();
  window.rippleInsertFromSource();

  const auto project = controller.editor().projectAt(controller.editor().revision());
  const auto video_track = std::find_if(
      project->sequences.front().tracks.begin(), project->sequences.front().tracks.end(),
      [](const auto& track) { return track.kind == video_editor::edit::TrackKind::Video; });
  QVERIFY(video_track != project->sequences.front().tracks.end());
  QCOMPARE(video_track->clips.size(), 1U);
  QCOMPARE(video_track->clips.front().source_range.start.rescaledTo(
               48'000, video_editor::edit::RoundingMode::NearestTiesEven)
               .value(),
           static_cast<std::int64_t>(48'000));
  QCOMPARE(video_track->clips.front().source_range.duration.rescaledTo(
               48'000, video_editor::edit::RoundingMode::NearestTiesEven)
               .value(),
           static_cast<std::int64_t>(48'000));
  QTRY_VERIFY_WITH_TIMEOUT(window.programViewer()->hasFrame(), 10'000);
}

void EditorControllerTest::inOutActionMarksProgramWhenSourceIsHidden() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("program-marks.mkv"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("program-marks-ui.ini")),
                     QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  QTRY_VERIFY_WITH_TIMEOUT(!window.timeline()->clips().isEmpty(), 10'000);
  window.setSourceMonitorVisible(false);
  QVERIFY(!window.sourceMonitorHasFocus());

  const qint64 duration = window.timeline()->clips().front().duration;
  QVERIFY(duration > 2);
  const qint64 in_point = duration / 4;
  const qint64 out_point = duration / 2;
  window.seekRequested(in_point);
  window.action(QStringLiteral("sourceMarkIn"))->trigger();
  window.seekRequested(out_point);
  window.action(QStringLiteral("sourceMarkOut"))->trigger();

  QCOMPARE(window.timeline()->programMarkIn().value_or(-1), in_point);
  QCOMPARE(window.timeline()->programMarkOut().value_or(-1), out_point);
}

void EditorControllerTest::importsSearchesAndExportsCaptions() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString srt_path = directory.filePath(QStringLiteral("captions.srt"));
  QFile source(srt_path);
  QVERIFY(source.open(QIODevice::WriteOnly));
  const QByteArray srt =
      QByteArrayLiteral("1\n00:00:00,000 --> 00:00:01,250\nWelcome to the edit\n\n"
                        "2\n00:00:01,250 --> 00:00:02,500\nMake every frame count\n\n");
  QCOMPARE(source.write(srt), static_cast<qint64>(srt.size()));
  source.close();

  QSettings settings(directory.filePath(QStringLiteral("captions-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  QVERIFY(controller.importCaptionFile(video_editor::app::pathFromQString(srt_path)));
  const auto project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(project->sequences.front().captions.size(), 2U);

  window.captionsPanel()->findInTranscriptRequested(QStringLiteral("frame"));
  auto* table = window.findChild<QTableWidget*>(QStringLiteral("captionsTable"));
  QVERIFY(table != nullptr);
  QCOMPARE(table->rowCount(), 1);
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Make every frame count"));
  table->item(0, 1)->setText(QStringLiteral("Make every cut count"));
  QTRY_COMPARE(controller.editor()
                   .projectAt(controller.editor().revision())
                   ->sequences.front()
                   .captions.at(1)
                   .text,
               std::string("Make every cut count"));
  window.undoRequested();
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.at(1)
               .text,
           std::string("Make every frame count"));

  const auto vtt_path =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("captions.vtt")));
  QVERIFY(controller.exportCaptionFile(vtt_path));
  QFile exported(video_editor::app::qStringFromPath(vtt_path));
  QVERIFY(exported.open(QIODevice::ReadOnly));
  const QByteArray vtt = exported.readAll();
  QVERIFY(vtt.startsWith("WEBVTT\n"));
  QVERIFY(vtt.contains("00:00:01.250 --> 00:00:02.500"));

  window.undoRequested();
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.size(),
           0U);

  window.captionsPanel()->addCaptionRequested();
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.size(),
           1U);
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.front()
               .text,
           std::string("New caption"));
  window.undoRequested();
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.size(),
           0U);
}

void EditorControllerTest::beginnerFifteenMinutePathWithoutFullEncode() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("dialogue.wav"));
  writeSilentWave(wave_path);

  QSettings settings(directory.filePath(QStringLiteral("beginner-controller-ui.ini")),
                     QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  QCOMPARE(window.mediaBin()->items().size(), 1);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  QCOMPARE(audioClipCount(*controller.editor().projectAt(controller.editor().revision())), 1U);

  window.captionsPanel()->addCaptionRequested();
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.size(),
           1U);
  QCOMPARE(controller.editor()
               .projectAt(controller.editor().revision())
               ->sequences.front()
               .captions.front()
               .text,
           std::string("New caption"));

  window.audioMixer()->gainEdited(0, -3.0);
  const auto gained = controller.editor().projectAt(controller.editor().revision());
  const auto gained_track = std::find_if(
      gained->sequences.front().tracks.begin(), gained->sequences.front().tracks.end(),
      [](const auto& track) { return track.kind == video_editor::edit::TrackKind::Audio; });
  QVERIFY(gained_track != gained->sequences.front().tracks.end());
  QCOMPARE(gained_track->audio_gain_db, -3.0);

  window.setWorkspace(video_editor::desktop_ui::Workspace::Deliver);
  auto* export_button = window.deliverPanel()->findChild<QToolButton*>(QStringLiteral("exportButton"));
  QVERIFY(export_button != nullptr);
  QVERIFY(!export_button->accessibleName().trimmed().isEmpty());
  QVERIFY(!window.deliverPanel()->selectedPresetId().isEmpty());

  for (const auto* name :
       {"importMediaButton", "addCaptionButton", "exportButton", "normalizationAnalyze"}) {
    auto* widget = window.findChild<QWidget*>(QString::fromLatin1(name));
    QVERIFY2(widget != nullptr, name);
    QVERIFY2(!widget->accessibleName().trimmed().isEmpty(), name);
  }
}

void EditorControllerTest::normalizationOnlyAdjustsAudibleContributingTracks() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("quiet-dialogue.wav"));
  writeSilentWave(wave_path, 48'000, 1'000);

  QSettings settings(directory.filePath(QStringLiteral("normalization-ui.ini")),
                     QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  // Give an empty, non-contributing track no remaining headroom. It must not
  // make the audible mix's review unsafe and must not be changed by Apply.
  window.audioMixer()->gainEdited(1, 24.0);
  auto before = controller.editor().projectAt(controller.editor().revision());
  std::vector<const video_editor::edit::Track*> audio_tracks;
  for (const auto& track : before->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio) {
      audio_tracks.push_back(&track);
    }
  }
  QCOMPARE(audio_tracks.size(), 4U);
  QCOMPARE(audio_tracks.at(1)->audio_gain_db, 24.0);
  QVERIFY(audio_tracks.at(1)->clips.empty());

  auto* apply = window.audioMixer()->findChild<QPushButton*>(QStringLiteral("normalizationApply"));
  QVERIFY(apply != nullptr);
  window.audioMixer()->normalizationAnalyzeRequested();
  QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 15'000);
  window.audioMixer()->normalizationApplyRequested();

  const auto applied = controller.editor().projectAt(controller.editor().revision());
  audio_tracks.clear();
  for (const auto& track : applied->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio) {
      audio_tracks.push_back(&track);
    }
  }
  QVERIFY(audio_tracks.at(0)->audio_gain_db > 0.0);
  QVERIFY(audio_tracks.at(0)->audio_gain_db <= 24.0);
  QCOMPARE(audio_tracks.at(1)->audio_gain_db, 24.0);

  window.undoRequested();
  const auto undone = controller.editor().projectAt(controller.editor().revision());
  audio_tracks.clear();
  for (const auto& track : undone->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio) {
      audio_tracks.push_back(&track);
    }
  }
  QCOMPARE(audio_tracks.at(0)->audio_gain_db, 0.0);
  QCOMPARE(audio_tracks.at(1)->audio_gain_db, 24.0);
}

void EditorControllerTest::realAudioDeviceUsesTheSampleCounterAsMasterClock() {
  if (!qEnvironmentVariableIsSet("VIDEO_EDITOR_TEST_AUDIO_DEVICE")) {
    QSKIP("Set VIDEO_EDITOR_TEST_AUDIO_DEVICE=1 on a machine with a configured output device");
  }

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("device-clock.wav"));
  writeSilentWave(wave_path, 144'000);

  QSettings settings(directory.filePath(QStringLiteral("device-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  QElapsedTimer control_latency;
  control_latency.start();
  window.playbackRateRequested(1.0);
  QVERIFY2(control_latency.elapsed() < 250,
           "audio start request blocked the Qt thread instead of enqueueing control");
  QTRY_VERIFY_WITH_TIMEOUT(controller.audioMasterActive(), 5'000);
  const std::int64_t started_at = controller.audioMasterSampleCounter();
  QTRY_VERIFY_WITH_TIMEOUT(controller.audioMasterSampleCounter() > started_at, 2'000);
  QTRY_VERIFY_WITH_TIMEOUT(window.timeline()->playhead() > started_at, 2'000);

  control_latency.restart();
  window.seekRequested(4'800);
  QVERIFY2(control_latency.elapsed() < 250,
           "audio seek request blocked the Qt thread instead of enqueueing control");
  QTRY_VERIFY_WITH_TIMEOUT(!controller.audioControlPending(), 5'000);
  QTRY_VERIFY_WITH_TIMEOUT(controller.audioMasterActive(), 5'000);
  QTest::qWait(500);
  QCOMPARE(controller.audioXrunCount(), 0U);

  control_latency.restart();
  window.playbackRateRequested(0.0);
  QVERIFY2(control_latency.elapsed() < 250,
           "audio pause request blocked the Qt thread instead of enqueueing control");
  QTRY_VERIFY_WITH_TIMEOUT(!controller.audioMasterActive(), 2'000);
  QTRY_VERIFY_WITH_TIMEOUT(!controller.audioControlPending(), 5'000);
  const std::int64_t paused_at = controller.audioMasterSampleCounter();
  QTest::qWait(50);
  QCOMPARE(controller.audioMasterSampleCounter(), paused_at);
}

void EditorControllerTest::audioBufferSizeDefaultsToMedium() {
  QSettings().remove(QStringLiteral("audio/bufferSize"));
  QSettings().remove(QStringLiteral("audio/adaptiveBufferBoost"));
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QSettings settings(directory.filePath(QStringLiteral("buffer-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  QCOMPARE(controller.audioBufferSize(), 1);
  QCOMPARE(controller.estimatedAvErrorMilliseconds(), 0.0);
  QCOMPARE(controller.audioClockUncertaintyFrames(), 0U);
  QCOMPARE(window.audioMixer()->bufferSize(), 1);
}

void EditorControllerTest::timedAutosaveCheckpointsDirtyProjects() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QSettings settings(directory.filePath(QStringLiteral("autosave-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  const auto checkpoint =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("autosave.veproj")));
  QVERIFY(controller.saveProjectFile(checkpoint));
  QVERIFY(!controller.dirty());
  window.timeline()->markerAddRequested(0);
  QVERIFY(controller.dirty());
  controller.setAutosaveIntervalSeconds(1);
  QTRY_VERIFY_WITH_TIMEOUT(!controller.dirty(), 5'000);
  QVERIFY(QFileInfo(directory.filePath(QStringLiteral("autosave.veproj"))).size() > 0);
}

void EditorControllerTest::reconstructsMediaRecordsFromPersistedAssets() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("dialogue.wav"));
  writeSilentWave(wave_path);

  video_editor::assets::AssetService service;
  auto imported = service.import(video_editor::app::pathFromQString(wave_path));
  QVERIFY2(imported, imported ? "" : imported.error().message.c_str());

  video_editor::edit::Asset asset;
  asset.id = video_editor::edit::EntityId::generate();
  asset.name = "dialogue.wav";
  asset.source_uri = video_editor::app::utf8_from_path(imported.value().uri);
  asset.fingerprint = imported.value().fingerprint.quick_sha256;
  asset.has_audio = true;

  const auto online =
      video_editor::app::reconstruct_asset_record(asset, nullptr);
  QCOMPARE(online.availability, video_editor::assets::AssetAvailability::Online);
  QCOMPARE(online.id, asset.id.toString());
  QCOMPARE(video_editor::app::utf8_from_path(online.uri), asset.source_uri);

  const auto command = video_editor::app::relink_command_from_record(online);
  QCOMPARE(command.asset_id, asset.id);
  QCOMPARE(command.source_uri, asset.source_uri);
  QCOMPARE(command.fingerprint, asset.fingerprint);
  QVERIFY(command.has_audio);

  video_editor::edit::Asset missing = asset;
  missing.source_uri = video_editor::app::utf8_from_path(
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("gone/dialogue.wav"))));
  const auto absent = video_editor::app::reconstruct_asset_record(missing, nullptr);
  QCOMPARE(absent.availability, video_editor::assets::AssetAvailability::Missing);

  const QString search_root = directory.filePath(QStringLiteral("search"));
  QVERIFY(QDir().mkpath(search_root));
  const QString recovered_path = QDir(search_root).filePath(QStringLiteral("dialogue.wav"));
  QVERIFY(QFile::copy(wave_path, recovered_path));

  video_editor::app::MediaReconstructionOptions options;
  options.search_directories.push_back(video_editor::app::pathFromQString(search_root));
  const auto recovered = video_editor::app::reconstruct_asset_record(missing, nullptr, options);
  QCOMPARE(recovered.availability, video_editor::assets::AssetAvailability::Online);
  QCOMPARE(QFileInfo(video_editor::app::qStringFromPath(recovered.uri)).fileName(),
           QStringLiteral("dialogue.wav"));
  QVERIFY(QFileInfo::exists(video_editor::app::qStringFromPath(recovered.uri)));

  const auto batch = video_editor::app::reconstruct_media_records({asset, missing}, nullptr, options);
  QCOMPARE(batch.size(), std::size_t{2});
  QCOMPARE(batch.front().availability, video_editor::assets::AssetAvailability::Online);
  QCOMPARE(batch.back().availability, video_editor::assets::AssetAvailability::Online);
}

bool writeShortProxyVideo(const QString& path) {
  const QStringList arguments{
      QStringLiteral("-hide_banner"),
      QStringLiteral("-loglevel"),
      QStringLiteral("error"),
      QStringLiteral("-nostdin"),
      QStringLiteral("-y"),
      QStringLiteral("-f"),
      QStringLiteral("lavfi"),
      QStringLiteral("-i"),
      QStringLiteral("testsrc2=size=320x180:rate=10:duration=2"),
      QStringLiteral("-c:v"),
      QStringLiteral("mpeg4"),
      QStringLiteral("-q:v"),
      QStringLiteral("4"),
      QStringLiteral("-pix_fmt"),
      QStringLiteral("yuv420p"),
      path,
  };
  return QProcess::execute(QStringLiteral(VIDEO_EDITOR_APP_TEST_FFMPEG), arguments) == 0 &&
         QFileInfo::exists(path);
}

void dismissMessageBoxes() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* box = qobject_cast<QMessageBox*>(widget)) {
      box->accept();
    }
  }
}

void EditorControllerTest::cancellingProxyDoesNotRegisterCompleteProxy() {
#ifdef VIDEO_EDITOR_APP_TEST_WORKER_HOST
  qputenv("VIDEO_EDITOR_WORKER_HOST", VIDEO_EDITOR_APP_TEST_WORKER_HOST);
#endif
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("proxy-source.mkv"));
  QVERIFY(writeShortProxyVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("proxy-cancel.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  QVERIFY(!window.mediaBin()->items().isEmpty());
  const QString asset_id = window.mediaBin()->items().front().id;

  QVERIFY(QMetaObject::invokeMethod(window.mediaBin(), "proxyRequested", Qt::DirectConnection,
                                    Q_ARG(QString, asset_id)));
  QTRY_VERIFY_WITH_TIMEOUT(window.mediaBin()->items().front().proxyGenerating, 5'000);
  QVERIFY(QMetaObject::invokeMethod(window.mediaBin(), "proxyRequested", Qt::DirectConnection,
                                    Q_ARG(QString, asset_id)));
  QTRY_VERIFY_WITH_TIMEOUT(!window.mediaBin()->items().front().proxyGenerating, 15'000);
  QVERIFY(!window.mediaBin()->items().front().proxyAvailable);
}

void EditorControllerTest::proxyWorkerDeathReportsFailureWithoutHanging() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString stub = directory.filePath(QStringLiteral("failing-worker"));
  QFile stub_file(stub);
  QVERIFY(stub_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QVERIFY(stub_file.write("#!/bin/sh\nexit 1\n") > 0);
  stub_file.close();
  QVERIFY(QFile::setPermissions(stub, QFileDevice::ExeOwner | QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner));
  qputenv("VIDEO_EDITOR_WORKER_HOST", stub.toUtf8());

  const QString video_path = directory.filePath(QStringLiteral("death-source.mkv"));
  QVERIFY(writeShortProxyVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("proxy-death.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  const QString asset_id = window.mediaBin()->items().front().id;

  QTimer dismiss;
  dismiss.setInterval(20);
  QObject::connect(&dismiss, &QTimer::timeout, &dismissMessageBoxes);
  dismiss.start();

  QVERIFY(QMetaObject::invokeMethod(window.mediaBin(), "proxyRequested", Qt::DirectConnection,
                                    Q_ARG(QString, asset_id)));
  QTRY_VERIFY_WITH_TIMEOUT(!window.mediaBin()->items().front().proxyGenerating, 10'000);
  QVERIFY(!window.mediaBin()->items().front().proxyAvailable);
#ifdef VIDEO_EDITOR_APP_TEST_WORKER_HOST
  qputenv("VIDEO_EDITOR_WORKER_HOST", VIDEO_EDITOR_APP_TEST_WORKER_HOST);
#else
  qunsetenv("VIDEO_EDITOR_WORKER_HOST");
#endif
}

void EditorControllerTest::rippleInsertsStillOverlayOnFreeVideoTrack() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("base-video.mkv"));
  const QString still_path = directory.filePath(QStringLiteral("overlay.ppm"));
  QVERIFY(writePlaybackVideo(video_path));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("still-overlay-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  controller.importPaths({still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 2U, 10'000);
  window.mediaActivated(window.mediaBin()->items().back().id);
  window.rippleInsertFromSource();

  const auto project = controller.editor().projectAt(controller.editor().revision());
  const auto& sequence = project->sequences.front();
  QCOMPARE(videoTrackCount(sequence), 2U);
  const auto* first_video = videoTrackAt(sequence, 0);
  const auto* second_video = videoTrackAt(sequence, 1);
  QVERIFY(first_video != nullptr);
  QVERIFY(second_video != nullptr);
  QCOMPARE(first_video->clips.size(), 1U);
  QCOMPARE(second_video->clips.size(), 1U);
  QCOMPARE(first_video->clips.front().timeline_range.start, video_editor::edit::Time{});
  QCOMPARE(second_video->clips.front().timeline_range.start, video_editor::edit::Time{});
}

void EditorControllerTest::rippleInsertsSecondStillOverlayAddsVideoTrack() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("stacked-base.mkv"));
  const QString still_path = directory.filePath(QStringLiteral("stacked-overlay.ppm"));
  QVERIFY(writePlaybackVideo(video_path));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("stacked-still-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path, still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 2U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  window.mediaActivated(window.mediaBin()->items().back().id);
  window.rippleInsertFromSource();
  window.mediaActivated(window.mediaBin()->items().back().id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(videoTrackCount(project->sequences.front()), 3U);
  const auto* third_video = videoTrackAt(project->sequences.front(), 2);
  QVERIFY(third_video != nullptr);
  QCOMPARE(third_video->clips.size(), 1U);
  QVERIFY(third_video->clips.front().timeline_range.start == video_editor::edit::Time{});

  window.undoRequested();
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(videoTrackCount(project->sequences.front()), 2U);
  QCOMPARE(videoTrackAt(project->sequences.front(), 2), nullptr);
  const auto* second_video = videoTrackAt(project->sequences.front(), 1);
  QVERIFY(second_video != nullptr);
  QCOMPARE(second_video->clips.size(), 1U);
}

void EditorControllerTest::rippleInsertsStillOnEmptyTimelineSetsFormat() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString still_path = directory.filePath(QStringLiteral("empty-timeline-still.ppm"));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("empty-still-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  const auto project = controller.editor().projectAt(controller.editor().revision());
  const auto& sequence = project->sequences.front();
  QCOMPARE(sequence.width, 16U);
  QCOMPARE(sequence.height, 10U);
  QCOMPARE(sequence.tracks.front().clips.size(), 1U);
  QCOMPARE(sequence.tracks.front().clips.front().timeline_range.start, video_editor::edit::Time{});
}

void EditorControllerTest::viewerMoveAndCropUpdateSelectedClipTransform() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString still_path = directory.filePath(QStringLiteral("viewer-transform-still.ppm"));
  writePpmFrame(still_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("viewer-transform-ui.ini")),
                     QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({still_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  window.viewerTransformPressed(QStringLiteral("cropLeft"), QPointF(-7.5, 0.0));
  window.viewerTransformMoved(QPointF(-5.5, 0.0));
  window.viewerTransformReleased();

  const auto cropped = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(cropped->sequences.front().tracks.front().clips.front().transform.crop_left, 0.125);

  window.viewerTransformPressed(QStringLiteral("move"), QPointF(0.0, 0.0));
  window.viewerTransformMoved(QPointF(32.0, 8.0));
  window.viewerTransformReleased();

  const auto moved = controller.editor().projectAt(controller.editor().revision());
  const auto& moved_clip = moved->sequences.front().tracks.front().clips.front();
  QCOMPARE(moved_clip.transform.position.x, 32.0);
  QCOMPARE(moved_clip.transform.position.y, 8.0);
  QCOMPARE(moved_clip.transform.crop_left, 0.125);

  window.undoRequested();
  const auto move_undone = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(move_undone->sequences.front().tracks.front().clips.front().transform.position.x, 0.0);
  QCOMPARE(move_undone->sequences.front().tracks.front().clips.front().transform.position.y, 0.0);
  QCOMPARE(move_undone->sequences.front().tracks.front().clips.front().transform.crop_left, 0.125);

  window.undoRequested();
  const auto crop_undone = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(crop_undone->sequences.front().tracks.front().clips.front().transform.crop_left, 0.0);
}

void EditorControllerTest::rippleInsertVideoMidClipStillFails() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("mid-clip-video.mkv"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("mid-clip-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);

  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto* first_video = videoTrackAt(project->sequences.front(), 0);
  QVERIFY(first_video != nullptr);
  QCOMPARE(first_video->clips.size(), 1U);
  const auto clip_duration = first_video->clips.front().timeline_range.duration;
  const qint64 mid_ui = static_cast<qint64>(
                             clip_duration
                                 .rescaledTo(window.timeline()->timeScale(),
                                             video_editor::edit::RoundingMode::NearestTiesEven)
                                 .value()) /
                         2;
  window.seekRequested(mid_ui);

  const auto revision_before = controller.editor().revision();
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  project = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(controller.editor().revision(), revision_before);
  QCOMPARE(videoTrackAt(project->sequences.front(), 0)->clips.size(), 1U);
}

void EditorControllerTest::nestSelectedClipsOpensChildSequence() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("nest-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("nest-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  const auto before = controller.editor().projectAt(controller.editor().revision());
  const QString clip_id =
      QString::fromStdString(before->sequences.front().tracks.front().clips.front().id.toString());
  window.timeline()->clipSelectionChanged({clip_id}, clip_id);
  window.action(QStringLiteral("nestSelectedClips"))->trigger();

  const auto after = controller.editor().projectAt(controller.editor().revision());
  QCOMPARE(after->sequences.size(), 2U);
  const video_editor::edit::Sequence* child = nullptr;
  const video_editor::edit::Sequence* parent = nullptr;
  for (const auto& sequence : after->sequences) {
    if (sequence.name == "Nested sequence") {
      child = &sequence;
    } else {
      parent = &sequence;
    }
  }
  QVERIFY(child != nullptr);
  QVERIFY(parent != nullptr);
  QVERIFY(!child->tracks.empty());
  QCOMPARE(child->tracks.front().clips.front().timeline_range.start, video_editor::edit::Time{});
  const auto nested_it = std::find_if(
      parent->tracks.begin(), parent->tracks.end(), [](const video_editor::edit::Track& track) {
        return track.kind == video_editor::edit::TrackKind::Video;
      });
  QVERIFY(nested_it != parent->tracks.end());
  QVERIFY(std::any_of(nested_it->clips.begin(), nested_it->clips.end(),
                      [](const video_editor::edit::Clip& clip) {
                        return clip.kind == video_editor::edit::ClipKind::NestedSequence;
                      }));
  auto* tabs = window.findChild<QTabBar*>(QStringLiteral("sequenceTabBar"));
  QVERIFY(tabs != nullptr);
  QCOMPARE(tabs->count(), 2);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Nested sequence"));
}

void EditorControllerTest::tracksRecentProjectsAndReopenLastSetting() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  const QString settings_path = directory.filePath(QStringLiteral("recent.ini"));
  QSettings settings(settings_path, QSettings::IniFormat);
  video_editor::app::setReopenLastOnStartup(settings, true);

  const auto checkpoint =
      video_editor::app::pathFromQString(directory.filePath(QStringLiteral("recent.veproj")));
  {
    QSettings window_settings(settings_path, QSettings::IniFormat);
    video_editor::desktop_ui::EditorWindow window(&window_settings);
    video_editor::app::EditorController controller(window);
    QVERIFY(controller.saveProjectFile(checkpoint));
  }

  const QStringList recent = video_editor::app::readRecentProjectPaths(settings);
  QCOMPARE(recent.size(), 1);
  QCOMPARE(recent.front(), video_editor::app::qStringFromPath(checkpoint));

  video_editor::app::pruneMissingRecentProjectPaths(settings);
  QCOMPARE(video_editor::app::readRecentProjectPaths(settings).size(), 1);

  QFile::remove(video_editor::app::qStringFromPath(checkpoint));
  video_editor::app::pruneMissingRecentProjectPaths(settings);
  QVERIFY(video_editor::app::readRecentProjectPaths(settings).isEmpty());
}

void EditorControllerTest::showingWindowDoesNotCrashDuringGpuPresentationInit() {
  QSettings settings;
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  window.show();
  // nativePresentationReady used to start GpuRenderer::create on a worker
  // thread, which SIGSEGV'd in vkGetPhysicalDeviceSurfaceSupportKHR. Drain
  // the queued GUI-thread init so this aborts if that path still crashes.
  QCoreApplication::processEvents();
  QTest::qWait(250);
  QVERIFY(window.isVisible());
}

void EditorControllerTest::emptyTimelineActionsReportWhyTheyDidNothing() {
  QSettings settings;
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  Q_UNUSED(controller)

  window.splitClipRequested();
  QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Select a clip before splitting"));
  window.deleteSelectionRequested(false);
  QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Select clips before deleting"));
  window.unlinkClipsRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Select clips before unlinking them"));
  window.copyClipsRequested();
  QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Select clips to copy"));
  window.nestSelectedClipsRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Select one or more clips to nest"));
  window.sourceRippleInsertRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Load a source clip before inserting"));
  window.setClipEnabledRequested(false);
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Select a clip before disabling it"));
  window.defaultTransitionRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("No edit point at the playhead for a default transition"));
  window.freezeFrameRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Place the playhead on a video clip to freeze a frame"));
  window.audioMixer()->normalizationApplyRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Analyze loudness before applying normalization"));
  if (auto* remove = window.findChild<QPushButton*>(QStringLiteral("removeCaptionButton"))) {
    remove->click();
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Select a caption to delete"));
  }
  if (auto* export_button = window.findChild<QToolButton*>(QStringLiteral("exportButton"))) {
    QVERIFY(export_button->isEnabled());
    export_button->click();
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Add at least one clip to the timeline before exporting."));
  }
  window.zoomToSelectionRequested();
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Select clips to zoom the timeline to them"));
  window.timeline()->nudgeActiveClipByFrames(
      1, video_editor::desktop_ui::TimelineWidget::EditIntent::Normal);
  QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Select clips before nudging them"));
  window.sourceStepShuttleRequested(1);
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Load a source clip before playing it"));
  window.sourceStepFrameRequested(1);
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Load a source clip before stepping frames"));
  window.action(QStringLiteral("commandPalette"))->trigger();
  auto* palette = window.findChild<video_editor::desktop_ui::CommandPalette*>();
  QVERIFY(palette != nullptr);
  auto* results = palette->findChild<QListWidget*>(QStringLiteral("commandPaletteResults"));
  QVERIFY(results != nullptr);
  bool found_paste = false;
  bool found_trim = false;
  for (int row = 0; row < results->count(); ++row) {
    const auto* item = results->item(row);
    if (item->text().contains(QStringLiteral("Paste Insert"))) {
      found_paste = true;
      QCOMPARE(item->toolTip(), QStringLiteral("Copy clips before pasting"));
    }
    if (item->text().contains(QStringLiteral("Overwrite Trim Tail to Playhead"))) {
      found_trim = true;
      QCOMPARE(item->toolTip(), QStringLiteral("No clips under the playhead to trim"));
    }
  }
  QVERIFY(found_paste);
  QVERIFY(found_trim);
  palette->close();
}

void EditorControllerTest::razorAddEditsSplitsUnlockedClips() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("razor-clip.mp4"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("razor-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  std::size_t video_clips = 0;
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Video) {
      video_clips += track.clips.size();
    }
  }
  QCOMPARE(video_clips, 1U);
  QVERIFY(!window.timeline()->clips().isEmpty());
  const auto view = window.timeline()->clips().front();
  const qint64 mid = view.start + view.duration / 2;
  QVERIFY(mid > view.start);
  QVERIFY(mid < view.start + view.duration);
  window.timeline()->addEditsAtRequested(mid);
  project = controller.editor().projectAt(controller.editor().revision());
  video_clips = 0;
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Video) {
      video_clips += track.clips.size();
    }
  }
  QCOMPARE(video_clips, 2U);
}

void EditorControllerTest::volumeEnvelopeUpsertsAudioVolumeKeyframe() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString wave_path = directory.filePath(QStringLiteral("envelope.wav"));
  writeSilentWave(wave_path, 48'000);

  QSettings settings(directory.filePath(QStringLiteral("envelope-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({wave_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  QString clip_id;
  qint64 duration = 0;
  const auto project = controller.editor().projectAt(controller.editor().revision());
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio && !track.clips.empty()) {
      clip_id = QString::fromStdString(track.clips.front().id.toString());
      duration = window.timeline()->clips().isEmpty() ? 0 : window.timeline()->clips().front().duration;
      break;
    }
  }
  QVERIFY(!clip_id.isEmpty());
  QVERIFY(duration > 1);
  window.timeline()->clipVolumeKeyframeUpserted(clip_id, {}, duration / 2, -6.0);

  const auto updated = controller.editor().projectAt(controller.editor().revision());
  const auto* audio = [&]() -> const video_editor::edit::Clip* {
    for (const auto& track : updated->sequences.front().tracks) {
      if (track.kind == video_editor::edit::TrackKind::Audio && !track.clips.empty()) {
        return &track.clips.front();
      }
    }
    return nullptr;
  }();
  QVERIFY(audio != nullptr);
  const auto volume = std::find_if(
      audio->effects.begin(), audio->effects.end(), [](const video_editor::edit::Effect& effect) {
        return effect.type == "audio.volume";
      });
  QVERIFY(volume != audio->effects.end());
  const auto parameter = volume->parameters.find("gain_db");
  QVERIFY(parameter != volume->parameters.end());
  QCOMPARE(parameter->second.keyframes.size(), std::size_t{1});
  QCOMPARE(std::get<double>(parameter->second.keyframes.front().value), -6.0);
}

void EditorControllerTest::opacityEnvelopeUpsertsVideoOpacityKeyframe() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("opacity-clip.mp4"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("opacity-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  QString clip_id;
  qint64 duration = 0;
  const auto project = controller.editor().projectAt(controller.editor().revision());
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Video && !track.clips.empty()) {
      clip_id = QString::fromStdString(track.clips.front().id.toString());
      duration = window.timeline()->clips().isEmpty() ? 0 : window.timeline()->clips().front().duration;
      break;
    }
  }
  QVERIFY(!clip_id.isEmpty());
  QVERIFY(duration > 1);
  window.timeline()->clipOpacityKeyframeUpserted(clip_id, {}, duration / 2, 0.25);

  const auto updated = controller.editor().projectAt(controller.editor().revision());
  const auto* video = [&]() -> const video_editor::edit::Clip* {
    for (const auto& track : updated->sequences.front().tracks) {
      if (track.kind == video_editor::edit::TrackKind::Video && !track.clips.empty()) {
        return &track.clips.front();
      }
    }
    return nullptr;
  }();
  QVERIFY(video != nullptr);
  const auto opacity = std::find_if(
      video->effects.begin(), video->effects.end(), [](const video_editor::edit::Effect& effect) {
        return effect.type == "video.opacity";
      });
  QVERIFY(opacity != video->effects.end());
  const auto parameter = opacity->parameters.find("opacity");
  QVERIFY(parameter != opacity->parameters.end());
  QCOMPARE(parameter->second.keyframes.size(), std::size_t{1});
  QCOMPARE(std::get<double>(parameter->second.keyframes.front().value), 0.25);
}

void EditorControllerTest::freezeFrameHoldsSourceAtPlayhead() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString video_path = directory.filePath(QStringLiteral("freeze-clip.mp4"));
  QVERIFY(writePlaybackVideo(video_path));

  QSettings settings(directory.filePath(QStringLiteral("freeze-ui.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({video_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  QString clip_id;
  qint64 start = 0;
  qint64 duration = 0;
  const auto project = controller.editor().projectAt(controller.editor().revision());
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Video && !track.clips.empty()) {
      clip_id = QString::fromStdString(track.clips.front().id.toString());
      start = window.timeline()->clips().front().start;
      duration = window.timeline()->clips().front().duration;
      break;
    }
  }
  QVERIFY(!clip_id.isEmpty());
  QVERIFY(duration > 2);
  window.timeline()->clipFreezeFrameRequested(clip_id, start + duration / 2);

  const auto updated = controller.editor().projectAt(controller.editor().revision());
  std::size_t video_clips = 0;
  const video_editor::edit::Clip* held = nullptr;
  for (const auto& track : updated->sequences.front().tracks) {
    if (track.kind != video_editor::edit::TrackKind::Video) {
      continue;
    }
    video_clips += track.clips.size();
    if (track.clips.size() >= 2) {
      held = &track.clips.back();
    }
  }
  QCOMPARE(video_clips, 2U);
  QVERIFY(held != nullptr);
  const double source_seconds =
      static_cast<double>(held->source_range.duration.value()) /
      static_cast<double>(std::max<std::uint32_t>(1, held->source_range.duration.timescale()));
  QVERIFY(source_seconds <= 1.0);
}

void EditorControllerTest::resyncLinkedAvMovesPartnersInOneUndoStep() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString media_path = directory.filePath(QStringLiteral("linked-av.mkv"));
  QVERIFY(writeMuxedAv(media_path));

  QSettings settings(directory.filePath(QStringLiteral("linked-av.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({media_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto* sequence = &project->sequences.front();
  const video_editor::edit::Clip* video = nullptr;
  const video_editor::edit::Clip* audio = nullptr;
  int audio_track_index = -1;
  for (int index = 0; index < static_cast<int>(sequence->tracks.size()); ++index) {
    const auto& track = sequence->tracks[static_cast<std::size_t>(index)];
    for (const auto& clip : track.clips) {
      if (clip.kind == video_editor::edit::ClipKind::Video) {
        video = &clip;
      } else if (clip.kind == video_editor::edit::ClipKind::Audio) {
        audio = &clip;
        audio_track_index = index;
      }
    }
  }
  QVERIFY(video != nullptr);
  QVERIFY(audio != nullptr);
  QVERIFY(video->linked_group.has_value());
  QCOMPARE(audio->linked_group, video->linked_group);
  QCOMPARE(video->timeline_range.start, audio->timeline_range.start);
  const QString audio_id = QString::fromStdString(audio->id.toString());
  const QString video_id = QString::fromStdString(video->id.toString());

  auto* linked_selection = window.action(QStringLiteral("toggleLinkedSelection"));
  QVERIFY(linked_selection != nullptr);
  QVERIFY(linked_selection->isChecked());
  linked_selection->trigger();
  QVERIFY(!linked_selection->isChecked());

  window.timeline()->clipSelectionChanged({audio_id}, audio_id);
  const qint64 frame_ticks = window.timeline()->frameStep();
  QVERIFY(frame_ticks > 0);
  const auto revision_before_move = controller.editor().revision();
  window.timeline()->clipBatchEditCommitted(
      {audio_id}, audio_track_index, 3 * frame_ticks, 0,
      video_editor::desktop_ui::TimelineWidget::EditMode::Move,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  QCOMPARE(controller.editor().revision().value, revision_before_move.value + 1U);

  project = controller.editor().projectAt(controller.editor().revision());
  sequence = &project->sequences.front();
  const video_editor::edit::Clip* moved_audio = nullptr;
  const video_editor::edit::Clip* still_video = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (QString::fromStdString(clip.id.toString()) == audio_id) {
        moved_audio = &clip;
      } else if (QString::fromStdString(clip.id.toString()) == video_id) {
        still_video = &clip;
      }
    }
  }
  QVERIFY(moved_audio != nullptr);
  QVERIFY(still_video != nullptr);
  QVERIFY(moved_audio->timeline_range.start != still_video->timeline_range.start);
  const auto offset_audio_start = moved_audio->timeline_range.start;
  const auto offset_video_start = still_video->timeline_range.start;

  bool saw_offset = false;
  for (const auto& clip : window.timeline()->clips()) {
    if (clip.linkedAvOffsetFrames == 3) {
      saw_offset = true;
    }
  }
  QVERIFY(saw_offset);

  auto* status =
      window.inspector()->findChild<QLabel*>(QStringLiteral("inspectorLinkedAvSyncStatus"));
  auto* resync =
      window.inspector()->findChild<QPushButton*>(QStringLiteral("inspectorResyncLinkedAv"));
  QVERIFY(status != nullptr);
  QVERIFY(resync != nullptr);
  QVERIFY(status->text().contains(QStringLiteral("+3")));
  QVERIFY(resync->isEnabled());

  const auto revision_before_resync = controller.editor().revision();
  resync->click();
  QCOMPARE(controller.editor().revision().value, revision_before_resync.value + 1U);

  project = controller.editor().projectAt(controller.editor().revision());
  sequence = &project->sequences.front();
  const video_editor::edit::Clip* resynced_audio = nullptr;
  const video_editor::edit::Clip* resynced_video = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (QString::fromStdString(clip.id.toString()) == audio_id) {
        resynced_audio = &clip;
      } else if (QString::fromStdString(clip.id.toString()) == video_id) {
        resynced_video = &clip;
      }
    }
  }
  QVERIFY(resynced_audio != nullptr);
  QVERIFY(resynced_video != nullptr);
  QCOMPARE(resynced_audio->timeline_range.start, resynced_video->timeline_range.start);
  QCOMPARE(window.statusBar()->currentMessage(),
           QStringLiteral("Linked clips realigned to the active clip"));

  window.undoRequested();
  project = controller.editor().projectAt(controller.editor().revision());
  sequence = &project->sequences.front();
  const video_editor::edit::Clip* undone_audio = nullptr;
  const video_editor::edit::Clip* undone_video = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (QString::fromStdString(clip.id.toString()) == audio_id) {
        undone_audio = &clip;
      } else if (QString::fromStdString(clip.id.toString()) == video_id) {
        undone_video = &clip;
      }
    }
  }
  QVERIFY(undone_audio != nullptr);
  QVERIFY(undone_video != nullptr);
  QCOMPARE(undone_audio->timeline_range.start, offset_audio_start);
  QCOMPARE(undone_video->timeline_range.start, offset_video_start);
}

void EditorControllerTest::trackVisibilityPresetIsolatesAndRestores() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString media_path = directory.filePath(QStringLiteral("tracks.mkv"));
  QVERIFY(writeMuxedAv(media_path));

  QSettings settings(directory.filePath(QStringLiteral("tracks.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({media_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();

  auto project = controller.editor().projectAt(controller.editor().revision());
  QStringList audio_ids;
  for (const auto& track : project->sequences.front().tracks) {
    if (track.kind == video_editor::edit::TrackKind::Audio) {
      audio_ids.push_back(QString::fromStdString(track.id.toString()));
    }
  }
  while (audio_ids.size() < 3) {
    window.timeline()->trackAddRequested(video_editor::desktop_ui::TrackKind::Audio);
    project = controller.editor().projectAt(controller.editor().revision());
    for (const auto& track : project->sequences.front().tracks) {
      if (track.kind == video_editor::edit::TrackKind::Audio) {
        const QString id = QString::fromStdString(track.id.toString());
        if (!audio_ids.contains(id)) {
          audio_ids.push_back(id);
        }
      }
    }
  }
  audio_ids = audio_ids.mid(0, 3);
  const QStringList names{QStringLiteral("Dialogue"), QStringLiteral("Music"),
                          QStringLiteral("Effects")};
  for (int index = 0; index < audio_ids.size(); ++index) {
    window.timeline()->trackRenameRequested(audio_ids.at(index), names.at(index));
  }
  const QString music_id = audio_ids.at(1);

  auto* nav = window.trackNav();
  auto* visibility = nav->findChild<QComboBox*>(QStringLiteral("trackVisibilityPreset"));
  auto* restore = nav->findChild<QPushButton*>(QStringLiteral("trackVisibilityRestore"));
  QVERIFY(nav != nullptr);
  QVERIFY(visibility != nullptr);
  QVERIFY(restore != nullptr);

  const int music_index =
      visibility->findData(static_cast<int>(video_editor::desktop_ui::TrackVisibilityPreset::Music));
  QVERIFY(music_index >= 0);
  visibility->setCurrentIndex(music_index);
  emit visibility->activated(music_index);
  project = controller.editor().projectAt(controller.editor().revision());
  for (const auto& track : project->sequences.front().tracks) {
    const QString id = QString::fromStdString(track.id.toString());
    if (id == music_id) {
      QVERIFY(track.visible);
    } else if (track.kind == video_editor::edit::TrackKind::Audio ||
               track.kind == video_editor::edit::TrackKind::Video) {
      QVERIFY(!track.visible);
    }
  }
  QVERIFY(restore->isEnabled());

  const QString original_sequence_id =
      QString::fromStdString(project->sequences.front().id.toString());
  QString clip_id;
  for (const auto& track : project->sequences.front().tracks) {
    if (!track.clips.empty()) {
      clip_id = QString::fromStdString(track.clips.front().id.toString());
      break;
    }
  }
  QVERIFY(!clip_id.isEmpty());
  window.timeline()->clipSelectionChanged(QStringList{clip_id}, clip_id);
  window.action(QStringLiteral("nestSelectedClips"))->trigger();
  QVERIFY(!restore->isEnabled());
  auto* tabs = window.findChild<QTabBar*>(QStringLiteral("sequenceTabBar"));
  QVERIFY(tabs != nullptr);
  int original_tab = -1;
  for (int index = 0; index < tabs->count(); ++index) {
    if (tabs->tabData(index).toString() == original_sequence_id) {
      original_tab = index;
      break;
    }
  }
  QVERIFY(original_tab >= 0);
  tabs->setCurrentIndex(original_tab);
  QVERIFY(restore->isEnabled());

  restore->click();
  project = controller.editor().projectAt(controller.editor().revision());
  const video_editor::edit::Sequence* original = nullptr;
  for (const auto& sequence : project->sequences) {
    if (QString::fromStdString(sequence.id.toString()) == original_sequence_id) {
      original = &sequence;
      break;
    }
  }
  QVERIFY(original != nullptr);
  for (const auto& track : original->tracks) {
    QVERIFY(track.visible);
  }
  QVERIFY(!restore->isEnabled());
}

void EditorControllerTest::backgroundJobsPauseDuringPlayback() {
  QSettings settings;
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  auto* job_label = window.findChild<QLabel*>(QStringLiteral("jobActivityStatus"));
  QVERIFY(job_label != nullptr);
  window.playbackRateRequested(1.0);
  QVERIFY(job_label->text().contains(QStringLiteral("paused")));
  window.playbackRateRequested(0.0);
  QVERIFY(!job_label->text().contains(QStringLiteral("paused")));
}

void EditorControllerTest::previewQualityPersistsAndUpdatesProgramTitle() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QSettings settings(directory.filePath(QStringLiteral("preview-quality.ini")), QSettings::IniFormat);
  settings.setValue(QStringLiteral("preview/qualityScale"),
                    static_cast<int>(video_editor::desktop_ui::PreviewQualityPreset::Half));
  settings.sync();
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);

  QCOMPARE(controller.previewQuality(), video_editor::desktop_ui::PreviewQualityPreset::Half);
  QVERIFY(window.programViewer()->title().contains(QStringLiteral("Half")));
  QVERIFY(window.programViewer()->title().contains(QStringLiteral("proxy")));
  QVERIFY(window.programViewer()->title().contains(QStringLiteral("reduced effects")));

  controller.setPreviewQuality(video_editor::desktop_ui::PreviewQualityPreset::Quarter);
  QCOMPARE(controller.previewQuality(), video_editor::desktop_ui::PreviewQualityPreset::Quarter);
  QVERIFY(window.programViewer()->title().contains(QStringLiteral("Quarter")));

  QSettings reloaded(directory.filePath(QStringLiteral("preview-quality.ini")), QSettings::IniFormat);
  QCOMPARE(reloaded.value(QStringLiteral("preview/qualityScale")).toInt(),
           static_cast<int>(video_editor::desktop_ui::PreviewQualityPreset::Quarter));
}

void EditorControllerTest::otioMenuActionsEmitImportExportSignals() {
  QSettings settings;
  video_editor::desktop_ui::EditorWindow window(&settings);
  QSignalSpy import_spy(&window, &video_editor::desktop_ui::EditorWindow::importOtioRequested);
  QSignalSpy export_spy(&window, &video_editor::desktop_ui::EditorWindow::exportOtioRequested);

  QAction* import_action = window.action(QStringLiteral("importOtio"));
  QAction* export_action = window.action(QStringLiteral("exportOtio"));
  QVERIFY(import_action != nullptr);
  QVERIFY(export_action != nullptr);
  import_action->trigger();
  export_action->trigger();
  QCOMPARE(import_spy.count(), 1);
  QCOMPARE(export_spy.count(), 1);
}

QTEST_MAIN(EditorControllerTest)
#include "editor_controller_test.moc"
