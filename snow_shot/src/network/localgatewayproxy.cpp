#include "localgatewayproxy.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QDebug>

LocalGatewayProxy::LocalGatewayProxy(QObject* parent)
    : QObject(parent),
      m_networkManager(new QNetworkAccessManager(this)),
      m_retryTimer(new QTimer(this)) {
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &LocalGatewayProxy::fetchLocalKey);
}

LocalGatewayProxy::~LocalGatewayProxy() {
    m_retryTimer->stop();
}

void LocalGatewayProxy::fetchLocalKey() {
    if (m_retryCount >= kMaxRetries) {
        emit localKeyFetchFailed(
            QStringLiteral("Cannot connect to Xiaoqiu Desktop local proxy. "
                           "Make sure Xiaoqiu Desktop is running and logged in."));
        return;
    }

    const QUrl url(QStringLiteral("%1/api/local-key").arg(QString::fromLatin1(kProxyBaseUrl)));
    QNetworkRequest request(url);
    request.setTransferTimeout(3000);

    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, &LocalGatewayProxy::onKeyFetchFinished);
}

void LocalGatewayProxy::onKeyFetchFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        ++m_retryCount;
        qWarning() << "Failed to fetch local key:" << reply->errorString()
                    << "Retry:" << m_retryCount;
        m_retryTimer->start(kRetryIntervalMs);
        return;
    }

    const QByteArray data = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonObject obj = doc.object();

    const QString key = obj.value(QStringLiteral("local_key")).toString();
    if (key.isEmpty()) {
        ++m_retryCount;
        qWarning() << "Local key is empty, retry:" << m_retryCount;
        m_retryTimer->start(kRetryIntervalMs);
        return;
    }

    m_localKey = key;
    m_retryCount = 0;
    qDebug() << "Successfully fetched local proxy key";
    emit localKeyFetched(key);
}

bool LocalGatewayProxy::hasValidKey() const {
    return !m_localKey.isEmpty();
}

QString LocalGatewayProxy::localKey() const {
    return m_localKey;
}

QString LocalGatewayProxy::proxyApiUrl() const {
    return QString::fromLatin1(kProxyBaseUrl);
}
