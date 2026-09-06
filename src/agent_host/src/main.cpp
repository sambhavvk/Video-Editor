// SPDX-License-Identifier: MPL-2.0

#include "video_editor/agent_host/agent_host.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <iostream>
#include <string>

namespace {

[[nodiscard]] bool writeResponse(const QJsonObject& response) {
  const QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
  QTextStream stream(stdout);
  stream << bytes << '\n';
  stream.flush();
  return stream.status() == QTextStream::Ok;
}

[[nodiscard]] QJsonObject readRequestFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    QJsonObject error;
    error.insert(QStringLiteral("ok"), false);
    QJsonObject details;
    details.insert(QStringLiteral("code"), QStringLiteral("io_error"));
    details.insert(QStringLiteral("message"), QStringLiteral("Could not read request file"));
    error.insert(QStringLiteral("error"), details);
    return error;
  }
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    QJsonObject error;
    error.insert(QStringLiteral("ok"), false);
    QJsonObject details;
    details.insert(QStringLiteral("code"), QStringLiteral("invalid_request"));
    details.insert(QStringLiteral("message"), QStringLiteral("Request file must contain a JSON object"));
    error.insert(QStringLiteral("error"), details);
    return error;
  }
  return document.object();
}

} // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("video_editor_agent_host"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Headless NDJSON host for agent edit commands"));
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption onceOption(QStringList{QStringLiteral("once")},
                                QStringLiteral("Read one request JSON file and exit"),
                                QStringLiteral("request.json"));
  parser.addOption(onceOption);
  parser.process(application);

  video_editor::agent_host::AgentHost host;

  if (parser.isSet(onceOption)) {
    const QJsonObject request = readRequestFile(parser.value(onceOption));
    if (request.contains(QStringLiteral("error"))) {
      return writeResponse(request) ? 1 : 1;
    }
    return writeResponse(host.handleRequest(request)) ? 0 : 1;
  }

  QTextStream input(stdin);
  while (true) {
    const QString line = input.readLine();
    if (line.isNull()) {
      break;
    }
    if (line.trimmed().isEmpty()) {
      continue;
    }
    const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8());
    if (!document.isObject()) {
      QJsonObject error;
      error.insert(QStringLiteral("ok"), false);
      QJsonObject details;
      details.insert(QStringLiteral("code"), QStringLiteral("invalid_request"));
      details.insert(QStringLiteral("message"), QStringLiteral("Each input line must be a JSON object"));
      error.insert(QStringLiteral("error"), details);
      if (!writeResponse(error)) {
        return 1;
      }
      continue;
    }
    if (!writeResponse(host.handleRequest(document.object()))) {
      return 1;
    }
  }

  return 0;
}
