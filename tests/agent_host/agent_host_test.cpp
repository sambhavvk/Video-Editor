// SPDX-License-Identifier: MPL-2.0

#include "video_editor/agent_host/agent_host.h"
#include "video_editor/agent_protocol/edit_command_json.h"
#include "video_editor/edit_model/commands.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace video_editor::agent_host {
namespace {

class QtApplicationEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    static int argc = 0;
    static char* argv[] = {nullptr};
    static QCoreApplication application(argc, argv);
    (void)application;
  }
};

const ::testing::Environment* const kQtEnvironment =
    ::testing::AddGlobalTestEnvironment(new QtApplicationEnvironment());

[[nodiscard]] std::string shell_quote(const std::filesystem::path& path) {
  const std::string value = path.string();
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

void write_ppm(const std::filesystem::path& path, const std::array<std::uint8_t, 3>& color) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output << "P6\n16 16\n255\n";
  for (int pixel = 0; pixel < 16 * 16; ++pixel) {
    output.write(reinterpret_cast<const char*>(color.data()),
                 static_cast<std::streamsize>(color.size()));
  }
  ASSERT_TRUE(output.good());
}

class DeterministicVideo final {
public:
  DeterministicVideo() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("video_editor_agent_host_" + edit::EntityId::generate().toString());
    std::filesystem::create_directories(directory_);
    constexpr std::array<std::array<std::uint8_t, 3>, 6> colors{{{255U, 0U, 0U},
                                                                 {255U, 0U, 0U},
                                                                 {0U, 255U, 0U},
                                                                 {0U, 255U, 0U},
                                                                 {0U, 0U, 255U},
                                                                 {0U, 0U, 255U}}};
    for (std::size_t index = 0; index < colors.size(); ++index) {
      std::array<char, 32> filename{};
      const int written = std::snprintf(filename.data(), filename.size(), "frame%03zu.ppm", index);
      if (written <= 0) {
        throw std::runtime_error("cannot format playback test filename");
      }
      write_ppm(directory_ / filename.data(), colors[index]);
    }

    video_path_ = directory_ / "fixture.mkv";
    const auto input_pattern = directory_ / "frame%03d.ppm";
    const std::string command =
        shell_quote(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -framerate 4 -i " + shell_quote(input_pattern) +
        " -frames:v 6 -c:v mpeg4 -g 6 -bf 2 -q:v 2 -pix_fmt yuv420p " + shell_quote(video_path_);
    if (std::system(command.c_str()) != 0 || !std::filesystem::is_regular_file(video_path_)) {
      throw std::runtime_error("ffmpeg could not create agent host test video");
    }
  }

  ~DeterministicVideo() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return video_path_;
  }

private:
  std::filesystem::path directory_;
  std::filesystem::path video_path_;
};

[[nodiscard]] const DeterministicVideo& fixture() {
  static const DeterministicVideo video;
  return video;
}

[[nodiscard]] QJsonObject request(AgentHost& host, const QJsonObject& body) {
  return host.handleRequest(body);
}

class AgentHostTest : public ::testing::Test {
protected:
  AgentHost host;
};

TEST_F(AgentHostTest, UnknownMethodFails) {
  QJsonObject body;
  body.insert(QStringLiteral("method"), QStringLiteral("not_a_real_method"));
  const QJsonObject response = request(host, body);
  EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool());
  EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code"))
                .toString(),
            QStringLiteral("unknown_method"));
}

TEST_F(AgentHostTest, ImportInsertAndPreviewProducesPng) {
  const QJsonObject created = request(host, QJsonObject{
                                              {QStringLiteral("method"), QStringLiteral("new_project")},
                                          });
  ASSERT_TRUE(created.value(QStringLiteral("ok")).toBool());

  const QJsonObject imported = request(host, QJsonObject{
                                                {QStringLiteral("method"), QStringLiteral("import_media")},
                                                {QStringLiteral("path"),
                                                 QString::fromStdString(fixture().path().string())},
                                            });
  ASSERT_TRUE(imported.value(QStringLiteral("ok")).toBool());

  const QJsonObject inserted = request(host, QJsonObject{
                                                 {QStringLiteral("method"), QStringLiteral("insert_media")},
                                                 {QStringLiteral("asset_id"),
                                                  imported.value(QStringLiteral("asset_id"))},
                                                 {QStringLiteral("mode"), QStringLiteral("ripple")},
                                             });
  ASSERT_TRUE(inserted.value(QStringLiteral("ok")).toBool());

  const auto preview_path =
      std::filesystem::temp_directory_path() /
      ("video_editor_agent_host_preview_" + edit::EntityId::generate().toString() + ".png");
  const QJsonObject preview = request(host, QJsonObject{
                                                {QStringLiteral("method"), QStringLiteral("get_preview_frame")},
                                                {QStringLiteral("output_path"),
                                                 QString::fromStdString(preview_path.string())},
                                            });
  ASSERT_TRUE(preview.value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(std::filesystem::is_regular_file(preview_path));
  EXPECT_GT(std::filesystem::file_size(preview_path), 0U);
  std::error_code ignored;
  std::filesystem::remove(preview_path, ignored);
}

TEST_F(AgentHostTest, ApplyCommandsRoundTrip) {
  const QJsonObject created = request(host, QJsonObject{
                                              {QStringLiteral("method"), QStringLiteral("new_project")},
                                          });
  ASSERT_TRUE(created.value(QStringLiteral("ok")).toBool());
  const qint64 revision = created.value(QStringLiteral("revision")).toInteger();

  const auto project = request(host, QJsonObject{{QStringLiteral("method"), QStringLiteral("read_session")}});
  ASSERT_TRUE(project.value(QStringLiteral("ok")).toBool());
  const QJsonArray sequences = project.value(QStringLiteral("project"))
                                   .toObject()
                                   .value(QStringLiteral("sequences"))
                                   .toArray();
  ASSERT_FALSE(sequences.isEmpty());
  const QString sequence_id =
      sequences.first().toObject().value(QStringLiteral("id")).toString();
  const QJsonArray tracks = sequences.first().toObject().value(QStringLiteral("tracks")).toArray();
  ASSERT_FALSE(tracks.isEmpty());
  const QString track_id = tracks.first().toObject().value(QStringLiteral("id")).toString();

  const edit::EditCommand lock_command{
      .operation = edit::SetTrackLockedCommand{
          .sequence_id = *edit::EntityId::parse(sequence_id.toStdString()),
          .track_id = *edit::EntityId::parse(track_id.toStdString()),
          .locked = true},
      .coalescing_key = {}};
  const QJsonObject encoded = agent_protocol::encode_command(lock_command);

  QJsonArray commands;
  commands.append(encoded);
  const QJsonObject applied = request(host, QJsonObject{
                                                {QStringLiteral("method"), QStringLiteral("apply_commands")},
                                                {QStringLiteral("expected_revision"), revision},
                                                {QStringLiteral("batch_name"), QStringLiteral("agent.test")},
                                                {QStringLiteral("commands"), commands},
                                            });
  ASSERT_TRUE(applied.value(QStringLiteral("ok")).toBool());
  EXPECT_GT(applied.value(QStringLiteral("revision")).toInteger(), revision);

  const QJsonObject session = request(host, QJsonObject{
                                                {QStringLiteral("method"), QStringLiteral("read_session")},
                                            });
  ASSERT_TRUE(session.value(QStringLiteral("ok")).toBool());
  const QJsonArray updated_tracks = session.value(QStringLiteral("project"))
                                        .toObject()
                                        .value(QStringLiteral("sequences"))
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value(QStringLiteral("tracks"))
                                        .toArray();
  EXPECT_TRUE(updated_tracks.first().toObject().value(QStringLiteral("locked")).toBool());
}

} // namespace
} // namespace video_editor::agent_host
