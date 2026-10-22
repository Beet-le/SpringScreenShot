#include "snow_shot/app/usagereportservice.h"

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace snow_shot::app {
namespace {
// PowerShell 7 is preferred, but the helper only requires 5.1, so fall back to the
// Windows PowerShell that ships with every supported Windows release.
const QStringList& shellCandidates() {
#ifdef Q_OS_WIN
    static const QStringList candidates = {QStringLiteral("pwsh.exe"),
                                           QStringLiteral("powershell.exe")};
#else
    static const QStringList candidates = {QStringLiteral("pwsh")};
#endif
    return candidates;
}
} // namespace

UsageReportService::UsageReportService(Options options, QObject* parent)
    : QObject(parent), m_options(std::move(options)) {
    m_scheduleTimer.setSingleShot(true);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &UsageReportService::runOnce);
}

UsageReportService::~UsageReportService() {
    m_scheduleTimer.stop();
}

void UsageReportService::start() {
    // A running schedule is left untouched; the daily pass re-reads the preference so
    // enabling or disabling takes effect on the next wake without a restart.
    if (!m_scheduleTimer.isActive())
        m_scheduleTimer.start(m_options.startupDelay);
}

void UsageReportService::stop() {
    m_scheduleTimer.stop();
}

bool UsageReportService::isEnabled() const {
    return storage::TelemetrySettings().dailyUsageUpload();
}

QString UsageReportService::resolveScriptPath() const {
    return QDir(QCoreApplication::applicationDirPath()).filePath(m_options.scriptFileName);
}

QString UsageReportService::resolveShell() const {
    for (const QString& candidate : shellCandidates()) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty())
            return found;
    }
    return {};
}

bool UsageReportService::dayNeedsUpload(const QString& logRoot, const QString& dayKey) const {
    if (logRoot.isEmpty() || dayKey.isEmpty())
        return false;
    const QDir root(logRoot);
    if (!root.exists(dayKey))
        return false; // no log directory means nothing was recorded that day
    const QString marker = QDir(root.filePath(QStringLiteral("submitted")))
                               .filePath(dayKey + QStringLiteral(".submitted"));
    return !QFileInfo::exists(marker); // the helper's own marker is authoritative
}

QString UsageReportService::resolveTargetDay(const QString& logRoot, const QDate& today) const {
    // Only ever consider complete previous days (offset >= 1); the running day is still
    // in progress and is uploaded on the next launch instead.
    for (int offset = 1; offset <= m_options.maxLookbackDays; ++offset) {
        const QString key = today.addDays(-offset).toString(QStringLiteral("yyyy-MM-dd"));
        if (dayNeedsUpload(logRoot, key))
            return key;
    }
    return {};
}

QStringList UsageReportService::buildArguments(const QString& scriptPath, const QString& logRoot,
                                               const QString& dateKey) {
    QStringList arguments;
    arguments << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
              << QStringLiteral("Bypass") << QStringLiteral("-WindowStyle")
              << QStringLiteral("Hidden") << QStringLiteral("-File") << scriptPath
              << QStringLiteral("-LogRoot") << logRoot << QStringLiteral("-Date") << dateKey;
    return arguments;
}

void UsageReportService::dispatchDay(const QString& logRoot, const QString& dayKey) {
    if (dayKey.isEmpty() || dayKey == m_lastDispatchedDay)
        return;
    if (!dayNeedsUpload(logRoot, dayKey))
        return;

    const QString scriptPath = resolveScriptPath();
    const QString shell = resolveShell();
    if (shell.isEmpty() || !QFileInfo::exists(scriptPath))
        return;

    m_lastDispatchedDay = dayKey;
    // Detached launch creates no pipes and shows no console window; the helper is
    // fully independent and any failure is silently ignored by design.
    static_cast<void>(QProcess::startDetached(shell, buildArguments(scriptPath, logRoot, dayKey),
                                              QCoreApplication::applicationDirPath()));
}

void UsageReportService::runOnce() {
    // Always re-arm for the next interval first, so any early return below still keeps
    // the schedule alive. Each pass uploads at most one day, so a long-running instance
    // steadily back-fills every missed previous day.
    m_scheduleTimer.start(m_options.interval);

    if (!isEnabled())
        return;

    const QString logRoot = diagnostics::DiagnosticsService::instance().status().directory;
    dispatchDay(logRoot, resolveTargetDay(logRoot, QDate::currentDate()));
}

} // namespace snow_shot::app
