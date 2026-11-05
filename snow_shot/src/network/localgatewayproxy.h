#ifndef SNOW_SHOT_LOCALGATEWAYPROXY_H
#define SNOW_SHOT_LOCALGATEWAYPROXY_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

/// Local gateway proxy client.
/// Automatically fetches the API key from Xiaoqiu Desktop's local proxy service
/// and configures the default AI model for zero-setup integration.
class LocalGatewayProxy : public QObject {
    Q_OBJECT
public:
    explicit LocalGatewayProxy(QObject* parent = nullptr);
    ~LocalGatewayProxy() override;

    static constexpr const char* kProxyBaseUrl = "http://localhost:18345";
    static constexpr const char* kDefaultModel = "qwen3.8-flash";

    void fetchLocalKey();
    [[nodiscard]] bool hasValidKey() const;
    [[nodiscard]] QString localKey() const;
    [[nodiscard]] QString proxyApiUrl() const;

signals:
    void localKeyFetched(const QString& key);
    void localKeyFetchFailed(const QString& error);

private slots:
    void onKeyFetchFinished();

private:
    QNetworkAccessManager* m_networkManager;
    QString m_localKey;
    QTimer* m_retryTimer;
    int m_retryCount = 0;
    static constexpr int kMaxRetries = 3;
    static constexpr int kRetryIntervalMs = 5000;
};

#endif
