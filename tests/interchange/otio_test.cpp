// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/otio.h"

#include <gtest/gtest.h>

#include <string>

namespace video_editor::interchange {
namespace {

edit::Project makeRoundtripProject() {
  edit::Project project;
  project.name = "Roundtrip Project";

  edit::Asset video_asset;
  video_asset.id = edit::EntityId::generate();
  video_asset.name = "clip-a.mov";
  video_asset.source_uri = "file:///media/clip-a.mov";
  video_asset.has_video = true;
  video_asset.has_audio = true;
  video_asset.duration = edit::Time(120, 30);
  project.assets.push_back(video_asset);

  edit::Asset audio_asset;
  audio_asset.id = edit::EntityId::generate();
  audio_asset.name = "voice.wav";
  audio_asset.source_uri = "file:///media/voice.wav";
  audio_asset.has_audio = true;
  audio_asset.duration = edit::Time(96'000, 48'000);
  project.assets.push_back(audio_asset);

  edit::Sequence nested;
  nested.id = edit::EntityId::generate();
  nested.name = "Nested Inner";
  nested.frame_rate = edit::Rate(30, 1);
  nested.width = 1920;
  nested.height = 1080;

  edit::Track nested_track;
  nested_track.id = edit::EntityId::generate();
  nested_track.kind = edit::TrackKind::Video;
  nested_track.name = "Nested V1";
  edit::Clip nested_clip;
  nested_clip.id = edit::EntityId::generate();
  nested_clip.asset_id = video_asset.id;
  nested_clip.kind = edit::ClipKind::Video;
  nested_clip.name = "Nested Clip";
  nested_clip.timeline_range = edit::TimeRange(edit::Time(0, 30), edit::Time(60, 30));
  nested_clip.source_range = nested_clip.timeline_range;
  nested_track.clips.push_back(nested_clip);
  nested.tracks.push_back(nested_track);
  project.sequences.push_back(nested);

  edit::Sequence main;
  main.id = edit::EntityId::generate();
  main.name = "Main Sequence";
  main.frame_rate = edit::Rate(30, 1);
  main.width = 1920;
  main.height = 1080;

  edit::Track video_track;
  video_track.id = edit::EntityId::generate();
  video_track.kind = edit::TrackKind::Video;
  video_track.name = "V1";
  edit::Clip gap_then_clip;
  gap_then_clip.id = edit::EntityId::generate();
  gap_then_clip.asset_id = video_asset.id;
  gap_then_clip.kind = edit::ClipKind::Video;
  gap_then_clip.timeline_range = edit::TimeRange(edit::Time(30, 30), edit::Time(60, 30));
  gap_then_clip.source_range = edit::TimeRange(edit::Time(0, 30), edit::Time(60, 30));
  video_track.clips.push_back(gap_then_clip);

  edit::Clip nested_on_timeline;
  nested_on_timeline.id = edit::EntityId::generate();
  nested_on_timeline.kind = edit::ClipKind::NestedSequence;
  nested_on_timeline.name = "Nested Comp";
  nested_on_timeline.nested_sequence_id = nested.id;
  nested_on_timeline.timeline_range = edit::TimeRange(edit::Time(120, 30), edit::Time(60, 30));
  nested_on_timeline.source_range = nested_on_timeline.timeline_range;
  video_track.clips.push_back(nested_on_timeline);
  main.tracks.push_back(video_track);

  edit::Track audio_track;
  audio_track.id = edit::EntityId::generate();
  audio_track.kind = edit::TrackKind::Audio;
  audio_track.name = "A1";
  edit::Clip audio_clip;
  audio_clip.id = edit::EntityId::generate();
  audio_clip.asset_id = audio_asset.id;
  audio_clip.kind = edit::ClipKind::Audio;
  audio_clip.timeline_range = edit::TimeRange(edit::Time(0, 48'000), edit::Time(48'000, 48'000));
  audio_clip.source_range = audio_clip.timeline_range;
  audio_track.clips.push_back(audio_clip);
  main.tracks.push_back(audio_track);

  edit::Marker marker;
  marker.id = edit::EntityId::generate();
  marker.label = "Review";
  marker.range = edit::TimeRange(edit::Time(45, 30), edit::Time(1, 30));
  main.markers.push_back(marker);

  project.sequences.push_back(main);
  return project;
}

TEST(OtioInterchangeTest, RoundTripPreservesTracksNestMarkersAndMediaUris) {
  const edit::Project original = makeRoundtripProject();
  const edit::Sequence* main_sequence = edit::findSequence(original, original.sequences.back().id);
  ASSERT_NE(main_sequence, nullptr);

  OtioReport export_report;
  const auto exported = export_otio_json(original, main_sequence->id, &export_report);
  ASSERT_TRUE(exported.hasValue()) << exported.error();

  OtioReport import_report;
  const auto imported = import_otio_json(exported.value(), &import_report);
  ASSERT_TRUE(imported.hasValue()) << imported.error();

  const edit::Project& project = imported.value();
  ASSERT_EQ(project.assets.size(), 2U);
  EXPECT_EQ(project.assets[0].source_uri, "file:///media/clip-a.mov");
  EXPECT_EQ(project.assets[1].source_uri, "file:///media/voice.wav");

  ASSERT_GE(project.sequences.size(), 1U);
  const edit::Sequence* root = nullptr;
  for (const edit::Sequence& candidate : project.sequences) {
    if (candidate.name == "Main Sequence") {
      root = &candidate;
      break;
    }
  }
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->tracks.size(), 2U);
  ASSERT_EQ(root->markers.size(), 1U);
  EXPECT_EQ(root->markers.front().label, "Review");

  const edit::Track& video = root->tracks[0];
  ASSERT_EQ(video.clips.size(), 2U);
  EXPECT_EQ(video.clips[0].timeline_range.start, edit::Time(30, 30));
  EXPECT_EQ(video.clips[1].kind, edit::ClipKind::NestedSequence);
  ASSERT_TRUE(video.clips[1].nested_sequence_id.has_value());
  const edit::Sequence* nested =
      edit::findSequence(project, *video.clips[1].nested_sequence_id);
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(nested->tracks.size(), 1U);
  EXPECT_EQ(nested->tracks[0].clips.size(), 1U);
  EXPECT_EQ(nested->tracks[0].clips[0].timeline_range.duration, edit::Time(60, 30));

  const edit::Track& audio = root->tracks[1];
  ASSERT_EQ(audio.clips.size(), 1U);
  EXPECT_EQ(audio.clips[0].source_range.duration, edit::Time(48'000, 48'000));
}

TEST(OtioInterchangeTest, SkipsUnknownEffectPluginsWithReport) {
  const char* json = R"({
    "OTIO_SCHEMA": "Timeline.1",
    "name": "Effect Timeline",
    "metadata": {
      "video_editor": {
        "width": 1920,
        "height": 1080,
        "frame_rate_numerator": 30,
        "frame_rate_denominator": 1
      }
    },
    "tracks": {
      "OTIO_SCHEMA": "Stack.1",
      "children": [
        {
          "OTIO_SCHEMA": "Track.1",
          "name": "V1",
          "kind": "Video",
          "children": [
            {
              "OTIO_SCHEMA": "SomeVendor.Effect.1",
              "name": "Mystery Effect"
            },
            {
              "OTIO_SCHEMA": "Clip.1",
              "name": "Safe Clip",
              "source_range": {
                "OTIO_SCHEMA": "TimeRange.1",
                "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 30},
                "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 30, "rate": 30}
              },
              "media_reference": {
                "OTIO_SCHEMA": "ExternalReference.1",
                "target_url": "file:///media/safe.mov"
              }
            }
          ]
        }
      ]
    },
    "markers": []
  })";

  OtioReport report;
  const auto imported = import_otio_json(json, &report);
  ASSERT_TRUE(imported.hasValue()) << imported.error();
  ASSERT_FALSE(report.skipped.empty());
  EXPECT_NE(report.skipped.front().find("SomeVendor.Effect.1"), std::string::npos);
  ASSERT_EQ(imported.value().sequences.front().tracks.front().clips.size(), 1U);
  EXPECT_EQ(imported.value().assets.front().source_uri, "file:///media/safe.mov");
}

TEST(OtioInterchangeTest, FailsClosedWhenClipHasNoMediaIdentity) {
  const char* json = R"({
    "OTIO_SCHEMA": "Timeline.1",
    "name": "Broken Timeline",
    "metadata": {
      "video_editor": {
        "width": 1920,
        "height": 1080,
        "frame_rate_numerator": 30,
        "frame_rate_denominator": 1
      }
    },
    "tracks": {
      "OTIO_SCHEMA": "Stack.1",
      "children": [
        {
          "OTIO_SCHEMA": "Track.1",
          "name": "V1",
          "kind": "Video",
          "children": [
            {
              "OTIO_SCHEMA": "Clip.1",
              "name": "Missing Media",
              "source_range": {
                "OTIO_SCHEMA": "TimeRange.1",
                "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 30},
                "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 30, "rate": 30}
              }
            }
          ]
        }
      ]
    },
    "markers": []
  })";

  const auto imported = import_otio_json(json);
  ASSERT_FALSE(imported.hasValue());
  EXPECT_NE(imported.error().find("ExternalReference"), std::string::npos);
}

}  // namespace
}  // namespace video_editor::interchange
