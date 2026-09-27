#pragma once
#include <QCommandLineParser>
#include <QStringList>

// Merge an optional INI file into an already parsed command line. Explicit
// command-line options win; file paths are relative to the INI directory.
bool applyServerConfig(QCommandLineParser& parser, const QStringList& arguments, QString& error);
