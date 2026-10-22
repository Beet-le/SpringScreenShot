#ifndef SNOW_SHOT_APP_USAGEREPORTSERVICE_H
#define SNOW_SHOT_APP_USAGEREPORTSERVICE_H

#include <QDate>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>

namespace snow_shot::app {

// Schedules the bundled submit-to-seq.ps1 helper so that daily usage summaries are
// uploaded to the production Seq endpoint without user interaction. The helper is
// launched as a detached, hidden child process, so the log parsing and HTTP upload
// never touch the UI thread or the application's performance budget.
//
// Uploads follow the work rhythm. The application is started manually and closed at
// the end of the day, so a tick only ever uploads the most recent *complete* previous
// day (never the running day, whose data is still incomplete). The current day is
// therefore sent on the next launch, once it has fully finished. The submitted marker
// written by the helper is the de-duplication source of truth.
class UsageReportService final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        std::chrono::milliseconds startupDelay = std::chrono::seconds(60);
        std::chrono::milliseconds interval = std::chrono::hours(1);
        QString scriptFileName = QStringLiteral("submit-to-seq.ps1");
        // How many past days to walk back when looking for an unuploaded complete day.
        int maxLookbackDays = 7;
    };

    explicit UsageReportService(Options options = {}, QObject* parent = nullptr);
    ~UsageReportService() override;

    // Arms the first delayed pass. Safe to call more than once; a running schedule is
    // never re-armed. The pass itself re-checks the enabled setting, so toggling the
    // preference takes effect without a restart.
    void start();
    void stop();

    [[nodiscard]] bool isEnabled() const;
    [[nodiscard]] QString resolveScriptPath() const;
    [[nodiscard]] QString resolveShell() const;
    // True when the given day has a log directory and has not been uploaded yet.
    [[nodiscard]] bool dayNeedsUpload(const QString& logRoot, const QString& dayKey) const;
    // Chooses the most recent complete previous day (never `today`) that still needs
    // uploading; returns an empty string when there is nothing appropriate to send.
    // Public and filesystem-driven so the selection can be verified deterministically.
    [[nodiscard]] QString resolveTargetDay(const QString& logRoot, const QDate& today) const;
    // Pure argument builder (everything after the shell executable) so it can be
    // verified without launching PowerShell.
    [[nodiscard]] static QStringList buildArguments(const QString& scriptPath,
                                                    const QString& logRoot, const QString& dateKey);

  private:
    void runOnce();
    void dispatchDay(const QString& logRoot, const QString& dayKey);

    Options m_options;
    QTimer m_scheduleTimer;
    QString m_lastDispatchedDay;
};

} // namespace snow_shot::app

#endif
