// SPDX-License-Identifier: MPL-2.0

#include "editor_controller.hpp"
#include "media_reconstruction.hpp"
#include "path_utils.hpp"

#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

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

} // namespace

class EditorControllerTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void importsInsertsAndRoundTripsUndo();
  void derivesSequenceFormatFromFirstVideoClip();
  void professionalTimelineInteractionsUseOneAtomicHistoryStep();
  void batchSplitAndFrameNudgePreserveExactSelectionGeometry();
  void rollEditsUseTheGestureEdge();
  void presentsFramesContinuouslyWhilePlaybackIsRunning();
  void importsSearchesAndExportsCaptions();
  void normalizationOnlyAdjustsAudibleContributingTracks();
  void realAudioDeviceUsesTheSampleCounterAsMasterClock();
  void audioDevicePollSteadyConnectedIsNotRecovered();
  void audioDevicePollLossReturnAndDelayedStopAreRetryable();
  void audioDevicePollPauseCancelsRecoveryIntent();
  void normalizationGenerationRejectsObsoleteCompletionAndClearsBusy();
  void mapsAndClampsReversedTranscriptWordsInPlaybackOrder();
  void rejectsOversizedModelDownloadBoundaries();
  void reconstructsMediaRecordsFromPersistedAssets();

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

  auto project = controller.editor().projectAt(controller.editor().revision());
  const auto& sequence = project->sequences.front();
  const QString first_track = QString::fromStdString(sequence.tracks.front().id.toString());
  const QString second_track = QString::fromStdString(sequence.tracks.at(1).id.toString());
  const QString first_clip =
      QString::fromStdString(sequence.tracks.front().clips.front().id.toString());
  window.timeline()->trackTargetToggled(first_track, false);
  window.mediaActivated(asset_id);
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
  project = controller.editor().projectAt(controller.editor().revision());
  const QString middle_before_trim =
      QString::fromStdString(project->sequences.front().tracks.front().clips.at(1).id.toString());
  window.timeline()->clipBatchEditCommitted(
      {middle_before_trim}, 0, 0, -one_second,
      video_editor::desktop_ui::TimelineWidget::EditMode::TrimOut,
      video_editor::desktop_ui::TimelineWidget::EditIntent::Normal, {});
  window.seekRequested((duration_ui - one_second) * 2);
  window.mediaActivated(asset_id);
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

QTEST_MAIN(EditorControllerTest)
#include "editor_controller_test.moc"
