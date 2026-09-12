// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/otio_handoff.h"

#include <fstream>
#include <system_error>
#include <unordered_set>

namespace video_editor::interchange {
namespace {

void write_line(std::ofstream& out, const std::string& line) {
  out << line << '\n';
}

[[nodiscard]] std::string clip_label(const edit::Clip& clip) {
  return clip.name.empty() ? clip.id.toString() : clip.name;
}

void collect_sequence_asset_uris(const edit::Project& project, const edit::Sequence& sequence,
                                 std::vector<std::string>& uris,
                                 std::unordered_set<edit::EntityId>& visited,
                                 std::unordered_set<std::string>& seen_uris) {
  if (!visited.insert(sequence.id).second) {
    return;
  }
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.kind == edit::ClipKind::NestedSequence && clip.nested_sequence_id.has_value()) {
        if (const edit::Sequence* nested =
                edit::findSequence(project, *clip.nested_sequence_id)) {
          collect_sequence_asset_uris(project, *nested, uris, visited, seen_uris);
        }
        continue;
      }
      const edit::Asset* asset = edit::findAsset(project, clip.asset_id);
      if (asset == nullptr || asset->source_uri.empty()) {
        continue;
      }
      if (seen_uris.insert(asset->source_uri).second) {
        uris.push_back(asset->source_uri);
      }
    }
  }
}

} // namespace

edit::Result<OtioHandoffPackage, std::string> build_otio_handoff_package(
    const edit::Project& project, const edit::EntityId sequence_id,
    const std::filesystem::path& destination_directory, const std::string& receiver_label) {
  if (destination_directory.empty()) {
    return edit::Result<OtioHandoffPackage, std::string>::failure("handoff destination is empty");
  }
  const edit::Sequence* sequence = edit::findSequence(project, sequence_id);
  if (sequence == nullptr) {
    return edit::Result<OtioHandoffPackage, std::string>::failure("sequence was not found");
  }

  OtioHandoffReport report;
  report.supported.push_back("OTIO Timeline JSON with ExternalReference media");
  report.supported.push_back("Sequence markers and clip timing");
  if (!receiver_label.empty()) {
    report.supported.push_back("Receiver: " + receiver_label);
  }
  for (const auto& track : sequence->tracks) {
    for (const auto& effect : track.effects) {
      if (!effect.known) {
        report.omitted.push_back("Unknown track effect: " + effect.type);
      } else {
        report.baked.push_back("Track effect metadata preserved: " + effect.type);
      }
    }
    for (const auto& clip : track.clips) {
      for (const auto& effect : clip.effects) {
        if (!effect.known) {
          report.omitted.push_back("Unknown clip effect on " + clip_label(clip) + ": " +
                                   effect.type);
        } else {
          report.baked.push_back("Clip effect metadata-only on " + clip_label(clip) + ": " +
                                 effect.type);
        }
      }
      if (clip.playback_rate != edit::Rate(1, 1) || clip.reversed) {
        report.omitted.push_back("Retime on " + clip_label(clip) +
                                 " is not exported as OTIO LinearTimeWarp");
      }
      if (clip.kind == edit::ClipKind::NestedSequence) {
        report.supported.push_back("Nested sequence exported as inline OTIO Stack: " +
                                   clip_label(clip));
      }
    }
  }

  OtioReport otio_report;
  const auto otio_json = export_otio_json(project, sequence_id, &otio_report);
  if (!otio_json) {
    return edit::Result<OtioHandoffPackage, std::string>::failure(otio_json.error());
  }
  report.otio = otio_report;
  for (const auto& skipped : otio_report.skipped) {
    report.omitted.push_back(skipped);
  }

  std::error_code ec;
  std::filesystem::create_directories(destination_directory, ec);
  if (ec) {
    return edit::Result<OtioHandoffPackage, std::string>::failure("could not create handoff folder");
  }

  OtioHandoffPackage package;
  package.package_root = destination_directory;
  package.timeline_path = destination_directory / "timeline.otio";
  package.report_path = destination_directory / "handoff_report.txt";
  package.manifest_path = destination_directory / "media_manifest.txt";

  {
    std::ofstream timeline(package.timeline_path);
    if (!timeline) {
      return edit::Result<OtioHandoffPackage, std::string>::failure("could not write timeline.otio");
    }
    timeline << otio_json.value();
  }
  {
    std::ofstream manifest(package.manifest_path);
    if (!manifest) {
      return edit::Result<OtioHandoffPackage, std::string>::failure("could not write media manifest");
    }
    std::vector<std::string> uris;
    std::unordered_set<edit::EntityId> visited;
    std::unordered_set<std::string> seen_uris;
    collect_sequence_asset_uris(project, *sequence, uris, visited, seen_uris);
    for (const auto& uri : uris) {
      write_line(manifest, uri);
    }
  }
  {
    std::ofstream handoff_report(package.report_path);
    if (!handoff_report) {
      return edit::Result<OtioHandoffPackage, std::string>::failure("could not write handoff report");
    }
    write_line(handoff_report, "OTIO handoff report");
    write_line(handoff_report, "Supported:");
    for (const auto& entry : report.supported) {
      write_line(handoff_report, "  - " + entry);
    }
    write_line(handoff_report, "Baked/metadata-only:");
    for (const auto& entry : report.baked) {
      write_line(handoff_report, "  - " + entry);
    }
    write_line(handoff_report, "Flattened:");
    for (const auto& entry : report.flattened) {
      write_line(handoff_report, "  - " + entry);
    }
    write_line(handoff_report, "Omitted:");
    for (const auto& entry : report.omitted) {
      write_line(handoff_report, "  - " + entry);
    }
  }

  package.report = std::move(report);
  return edit::Result<OtioHandoffPackage, std::string>::success(std::move(package));
}

} // namespace video_editor::interchange
