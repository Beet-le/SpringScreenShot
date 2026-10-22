#include "../include/snow_shot/app/usagereportservice.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

using snow_shot::app::UsageReportService;

QString dayKey(const QDate& date) {
    return date.toString(QStringLiteral("yyyy-MM-dd"));
}

void makeLogDay(const QString& logRoot, const QDate& date) {
    static_cast<void>(QDir(logRoot).mkpath(dayKey(date)));
}

void makeSubmittedMarker(const QString& logRoot, const QDate& date) {
    const QString dir = QDir(logRoot).filePath(QStringLiteral("submitted"));
    static_cast<void>(QDir(logRoot).mkpath(QStringLiteral("submitted")));
    QFile marker(QDir(dir).filePath(dayKey(date) + QStringLiteral(".submitted")));
    require(marker.open(QIODevice::WriteOnly), "create a submitted marker fixture");
    marker.close();
}

void argumentBuilderTargetsOneExplicitDay() {
    const QStringList arguments =
        UsageReportService::buildArguments(QStringLiteral("C:/apps/bin/submit-to-seq.ps1"),
                                           QStringLiteral("C:/logs"), QStringLiteral("2026-10-20"));
    require(arguments.contains(QStringLiteral("-NoProfile")),
            "the helper must run without loading user profiles");
    require(arguments.contains(QStringLiteral("-WindowStyle")) &&
                arguments.contains(QStringLiteral("Hidden")),
            "the helper must launch without a visible console window");
    require(arguments.contains(QStringLiteral("-ExecutionPolicy")) &&
                arguments.contains(QStringLiteral("Bypass")),
            "the helper must bypass execution-policy prompts");
    const qsizetype fileIndex = arguments.indexOf(QStringLiteral("-File"));
    require(fileIndex >= 0 &&
                arguments.value(fileIndex + 1) == QStringLiteral("C:/apps/bin/submit-to-seq.ps1"),
            "the script path must follow the -File switch");
    const qsizetype logIndex = arguments.indexOf(QStringLiteral("-LogRoot"));
    require(logIndex >= 0 && arguments.value(logIndex + 1) == QStringLiteral("C:/logs"),
            "the active log root must follow the -LogRoot switch");
    const qsizetype dateIndex = arguments.indexOf(QStringLiteral("-Date"));
    require(dateIndex >= 0 && arguments.value(dateIndex + 1) == QStringLiteral("2026-10-20"),
            "the scheduler must pin the exact day to upload with -Date");
}

void scriptPathResolvesBesideTheExecutable() {
    UsageReportService::Options options;
    options.scriptFileName = QStringLiteral("submit-to-seq.ps1");
    const UsageReportService service(options);
    const QString expected =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("submit-to-seq.ps1"));
    require(service.resolveScriptPath() == expected,
            "the helper must be located next to the application executable");
}

void targetDayFollowsTheWorkRhythm() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary log root must be creatable");
    const QString logRoot = directory.path();
    const UsageReportService service;

    const QDate today = QDate::currentDate();
    const QDate yesterday = today.addDays(-1);
    makeLogDay(logRoot, today);
    makeLogDay(logRoot, yesterday);

    // The running day is never uploaded; the most recent complete previous day is.
    require(service.resolveTargetDay(logRoot, today) == dayKey(yesterday),
            "a tick must catch up the previous complete day, never the partial current day");

    // Once the previous day is uploaded there is nothing older present, so nothing is sent.
    makeSubmittedMarker(logRoot, yesterday);
    require(service.resolveTargetDay(logRoot, today).isEmpty(),
            "ticks stop once earlier complete days are already uploaded");

    // Even when only the running day has logs, it must never be selected.
    QTemporaryDir onlyToday;
    require(onlyToday.isValid(), "temporary log root must be creatable");
    makeLogDay(onlyToday.path(), today);
    require(service.resolveTargetDay(onlyToday.path(), today).isEmpty(),
            "the running day alone is never uploaded, even if it is the only log present");

    require(service.resolveTargetDay(QString(), today).isEmpty(),
            "an empty log root never selects a day");
}

void shellResolutionPrefersAnAvailablePowerShell() {
    const UsageReportService service;
#ifdef Q_OS_WIN
    require(!service.resolveShell().isEmpty(),
            "a Windows host always ships Windows PowerShell for the fallback");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    argumentBuilderTargetsOneExplicitDay();
    scriptPathResolvesBesideTheExecutable();
    targetDayFollowsTheWorkRhythm();
    shellResolutionPrefersAnAvailablePowerShell();
    return 0;
}
