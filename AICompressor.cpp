#include "AICompressor.h"
#include <QUrl>
#include <QTimer>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

AICompressor* AICompressor::_instance = nullptr;
bool AICompressor::isNewSession = true;

AICompressor::AICompressor(QObject *parent)
    : QObject(parent),
    cmdIDCounter(1),
    hasCameras(false),
    _bitrate(1000),
    _fps(30),
    _scaling(100),
    _isReconnecting(false),
    _shouldReconnect(true)
{
#ifdef XBFIRM
    _generalSettings = Settings::generalSetting();
    serverUrl = QString("ws://%1:%2").arg(_generalSettings->aiCompressorHost()).arg(QString::number(_generalSettings->aiCompressorPort()));
    _cameraInputPipeline = _generalSettings->aiCompressorDefaultInputPipeline();
    _cameraOutputPipeline = _generalSettings->aiCompressorOutputPipeline();
#else
    serverUrl = QString("ws://%1:%2").arg(AI_COMPRESSOR_DEFAULT_HOST).arg(QString::number(AI_COMPRESSOR_DEFAULT_PORT));
    _cameraInputPipeline = AI_COMPRESSOR_DEFAULT_INPUT_PIPELINE;
    _cameraOutputPipeline = AI_COMPRESSOR_DEFAULT_OUTPUT_PIPELINE;
#endif
    userName = AI_COMPRESSOR_ADMIN;
    password = AI_COMPRESSOR_PASSWORD;

    qDebug() << "AICompressor: initializing. server =" << serverUrl;

    webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(webSocket, &QWebSocket::connected, this, &AICompressor::onConnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &AICompressor::onMessageReceived);
    connect(webSocket, &QWebSocket::disconnected, this, &AICompressor::onDisconnected);

    // Thêm error handling - dùng signal error() thay vì errorOccurred() cho Qt 5
    connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &AICompressor::onError);

    connect(&_getInfoTimer, &QTimer::timeout, this, &AICompressor::handleGetInfoTimer);
    _getInfoTimer.setSingleShot(false);

    // THÊM: Timer cho reconnect
    _reconnectTimer = new QTimer(this);
    _reconnectTimer->setSingleShot(true);
    connect(_reconnectTimer, &QTimer::timeout, this, &AICompressor::attemptReconnect);

    // this->start(_cameraInputPipeline);
}

AICompressor::~AICompressor()
{
    if (webSocket)
        webSocket->deleteLater();
}

AICompressor *AICompressor::instance()
{
    if (_instance == nullptr) {
        _instance = new AICompressor();
    }
    return _instance;
}

void AICompressor::start(QString pipeline)
{
    AICompressor::isNewSession = true;
    _cameraInputPipeline = pipeline;
    _shouldReconnect = true;  // THÊM: Bật reconnect
    this->setCameraStatus("");
    this->connectToServer();

    qDebug() << "AICompressor: Start ver2 "<<_cameraInputPipeline;
}

void AICompressor::stop()
{
    qDebug() << "AICompressor: stop requested";
    _shouldReconnect = false;  // THÊM: Tắt reconnect
    
    // THÊM: Stop reconnect timer if running
    if (_reconnectTimer && _reconnectTimer->isActive()) {
        _reconnectTimer->stop();
        qDebug() << "AICompressor: stopped reconnect timer";
    }
    
    // Stop info timer
    if (_getInfoTimer.isActive()) {
        _getInfoTimer.stop();
    }
    
    this->deleteCamera();
    this->deleteCamera();
    this->closeConnection();
}

QString AICompressor::getOutputRtspURl()
{
    return _cameraOutputPipeline;
}

void AICompressor::handleChangeBitrate(int bitrate)
{
    this->updateBitrate(bitrate);
}

void AICompressor::handleChangeFps(int fps)
{
    Q_UNUSED(fps)
}

void AICompressor::handleChangeScale(int scale)
{
    Q_UNUSED(scale)
}

void AICompressor::handleGetInfoTimer()
{
    this->getCameras();
}

void AICompressor::connectToServer()
{
    if (webSocket->state() == QAbstractSocket::ConnectedState) {
        qDebug() << "AICompressor: already connected to" << serverUrl;
        return;
    }

    if (webSocket->state() == QAbstractSocket::ConnectingState) {
        qDebug() << "AICompressor: connection in progress to" << serverUrl;
        return;
    }

    qDebug() << "AICompressor: attempting WebSocket connection to" << serverUrl;
    qDebug() << "AICompressor: current state =" << webSocket->state();

    webSocket->open(QUrl(serverUrl));

    // Log warning sau 5 giây nếu chưa connect
    QTimer::singleShot(5000, this, [this]() {
        if (webSocket->state() != QAbstractSocket::ConnectedState) {
            qWarning() << "AICompressor: ✗ CONNECTION TIMEOUT after 5 seconds";
            qWarning() << "AICompressor: current state =" << webSocket->state();
            qWarning() << "AICompressor: server URL =" << serverUrl;
            qWarning() << "AICompressor: last error =" << webSocket->errorString();
        }
    });
}

void AICompressor::getCameras()
{
    if (!webSocket) {
        qCritical() << "AICompressor: ✗ Cannot get cameras - webSocket is NULL";
        return;
    }

    if (webSocket->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "AICompressor: ✗ Cannot get cameras - NOT CONNECTED";
        qWarning() << "AICompressor: current state:" << webSocket->state();
        return;
    }

    QString getCamerasCommand = createGetCamerasCommand();
    qDebug() << "AICompressor: requesting camera list";
    webSocket->sendTextMessage(getCamerasCommand);
}

void AICompressor::onConnected()
{
    qDebug() << "AICompressor: ✓✓✓ WebSocket CONNECTED successfully ✓✓✓";
    qDebug() << "AICompressor: server:" << serverUrl;
    qDebug() << "AICompressor: connection state =" << webSocket->state();
    
    // THÊM: Reset reconnect state on successful connection
    _isReconnecting = false;
    if (_reconnectTimer && _reconnectTimer->isActive()) {
        _reconnectTimer->stop();
    }
    
    qDebug() << "AICompressor: preparing login command with username:" << userName;

    QString loginCommand = createLoginCommand(userName, password);
    qDebug() << "AICompressor: login command generated:" << loginCommand;
    qDebug() << "AICompressor: sending login command";

    webSocket->sendTextMessage(loginCommand);
    qDebug() << "AICompressor: login command sent successfully";
}

void AICompressor::onMessageReceived(const QString &message)
{
    qDebug() << "AICompressor: message received:" << message;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        qWarning() << "AICompressor: invalid JSON message or parse error:" << err.errorString();
        return;
    }

    QJsonObject obj = doc.object();
    int replyId = obj.value("replyId").toInt(-1);
    QString event = obj.value("event").toString();
    QJsonObject replyObject = obj.value("reply").toArray().at(0).toObject();

    if (event == "updateCamera") {
        QJsonObject dataObject = obj.value("data").toObject();
        QString status = dataObject.value("status").toString();
        if (!status.isEmpty()) {
            this->setCameraStatus(status);
        }
    }

    if (!replyObject.isEmpty()) {
        int replyObjectId = replyObject.value("id").toInt();
        QJsonObject outputObject = replyObject.value("output").toObject();
        if (replyObjectId>0 && !outputObject.isEmpty()) {
            double inputBandwidth = outputObject.value("actualKbps").toDouble();
            double outputBandwidth = outputObject.value("kbps").toDouble();
            emit this->onOutputStat(inputBandwidth/8, outputBandwidth/8);
        }
        QString error = replyObject.value("error").toString();
        if (!error.isEmpty()) {
            if (error.startsWith("Camera Error: No video being received")) {
                qDebug() << "--> Camera Error: No video being received, refresh camera";
                this->refreshCamera();
            }
            else if (error.startsWith("Camera Error:")) {
                qDebug() << "--> Camera Error: "<<error;
                this->refreshCamera();
            }
        }
    }

    CommandID cmdId = static_cast<CommandID>(replyId);
    emit sendMessage(message);
    qDebug() << "AICompressor: replyId =" << replyId << "interpreted cmdId =" << static_cast<int>(cmdId);

    switch (cmdId)
    {
    case CommandID::deleteCameraCommand:
        qDebug() << "AICompressor: deleteCameraCommand";
        this->createNewCamera("xbstream");
        break;
    case CommandID::createLoginCommand:
        qDebug() << "AICompressor: login acknowledged, requesting camera list.";
        getCameras();
        _shouldReconnect = false;
        break;
    case CommandID::createGetCamerasCommand:
        qDebug() << "AICompressor: processing getCameras response.";
        processGetCamerasResponse(message);
        break;
    case CommandID::createCreateCameraCommand:
        qDebug() << "AICompressor: camera created, refreshing camera list.";
        if (_getInfoTimer.isActive()) {
            _getInfoTimer.stop();
        }
        _getInfoTimer.start(3000);
        getCameras();
        break;
    default:
        qDebug() << "AICompressor: unhandled cmdId =" << static_cast<int>(cmdId);
        break;
    }
}

void AICompressor::onDisconnected()
{
    qWarning() << "AICompressor: ✗✗✗ WebSocket DISCONNECTED ✗✗✗";
    qWarning() << "AICompressor: server was:" << serverUrl;
    qWarning() << "AICompressor: disconnect reason:" << webSocket->closeReason();
    qWarning() << "AICompressor: close code:" << webSocket->closeCode();
    qWarning() << "AICompressor: state after disconnect:" << webSocket->state();
    qWarning() << "AICompressor: last error:" << webSocket->errorString();

    if (_getInfoTimer.isActive()) {
        _getInfoTimer.stop();
        qDebug() << "AICompressor: stopped info timer due to disconnection";
    }

    qDebug() << "AICompressor: cameras count at disconnect:" << cameraNames.size();
    qDebug() << "AICompressor: camera status:" << _cameraStatus;
    qDebug() << "AICompressor: selected camera:" << selectedCameraName;
    
    // THÊM: Auto reconnect nếu không phải user stop
    if (_shouldReconnect && !_isReconnecting) {
        qWarning() << "AICompressor: will attempt reconnect in 5 seconds...";
        scheduleReconnect(5000);
    }
}

void AICompressor::onError(QAbstractSocket::SocketError error)
{
    qCritical() << "AICompressor: ✗✗✗ WebSocket ERROR ✗✗✗";
    qCritical() << "AICompressor: error code:" << error;
    qCritical() << "AICompressor: error string:" << webSocket->errorString();
    qCritical() << "AICompressor: server URL:" << serverUrl;
    qCritical() << "AICompressor: current state:" << webSocket->state();

    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        qCritical() << "AICompressor: Connection REFUSED - server not accepting connections";
        qCritical() << "AICompressor: Check if AI Compressor service is running";
#ifdef XBFIRM
        qCritical() << "AICompressor: configured host:" << _generalSettings->aiCompressorHost();
        qCritical() << "AICompressor: configured port:" << _generalSettings->aiCompressorPort();
#else
        qCritical() << "AICompressor: configured host:" << AI_COMPRESSOR_DEFAULT_HOST;
        qCritical() << "AICompressor: configured port:" << AI_COMPRESSOR_DEFAULT_PORT;
#endif
        break;
    case QAbstractSocket::RemoteHostClosedError:
        qCritical() << "AICompressor: Remote host CLOSED the connection";
        break;
    case QAbstractSocket::HostNotFoundError:
        qCritical() << "AICompressor: Host NOT FOUND - check server address";
#ifdef XBFIRM
        qCritical() << "AICompressor: configured host:" << _generalSettings->aiCompressorHost();
#else
        qCritical() << "AICompressor: configured host:" << AI_COMPRESSOR_DEFAULT_HOST;
#endif
        break;
    case QAbstractSocket::SocketTimeoutError:
        qCritical() << "AICompressor: Connection TIMEOUT";
        break;
    case QAbstractSocket::NetworkError:
        qCritical() << "AICompressor: NETWORK error - check network connectivity";
        break;
    case QAbstractSocket::SocketAccessError:
        qCritical() << "AICompressor: Socket ACCESS error - check permissions";
        break;
    default:
        qCritical() << "AICompressor: Unknown error type:" << error;
        break;
    }

    emit sendMessage(QString("AI Compressor Error: %1").arg(webSocket->errorString()));
    
    // THÊM: Auto reconnect nếu shouldReconnect = true
    if (_shouldReconnect && !_isReconnecting) {
        qWarning() << "AICompressor: Error occurred, will attempt reconnect in 5 seconds...";
        scheduleReconnect(5000);
    }
}

void AICompressor::sendCommand(QString command)
{
    if (!webSocket) {
        qCritical() << "AICompressor: ✗ Cannot send command - webSocket is NULL";
        return;
    }

    if (webSocket->state() != QAbstractSocket::ConnectedState) {
        qCritical() << "AICompressor: ✗ Cannot send command - NOT CONNECTED";
        qCritical() << "AICompressor: current state:" << webSocket->state();
        qCritical() << "AICompressor: command was:" << command.left(100) << "...";
        return;
    }

    qDebug() << "AICompressor: sending command:" << command.left(100) << "...";
    webSocket->sendTextMessage(command);
}

// ================== Command Generators ==================

QString AICompressor::createLoginCommand(QString &username, QString &password)
{
    return QString(R"({"cmd":"login","username":"%1","password":"%2","cmdId":%3})")
    .arg(username)
        .arg(password)
        .arg(static_cast<int>(CommandID::createLoginCommand));
}

QString AICompressor::createGetCamerasCommand()
{
    return QString(R"({"cmd":"getCameras","cmdId":%1})")
    .arg(static_cast<int>(CommandID::createGetCamerasCommand));
}

QString AICompressor::deleteCameraCommand(int cameraId)
{
    return QString(R"({"cmd":"deleteCamera", "id": %2,"cmdId":%1})")
    .arg(static_cast<int>(CommandID::deleteCameraCommand))
        .arg(QString::number(cameraId));
}

QString AICompressor::createCreateCameraCommand(const QString &cameraName,
                                                const QString &url,
                                                int bitrate,
                                                int scaling,
                                                int fps)
{
    qDebug() << "AICompressor: createCreateCameraCommand called with parameters:";
    qDebug() << "  - cameraName:" << cameraName;
    qDebug() << "  - url:" << url;
    qDebug() << "  - _cameraInputPipeline:" << _cameraInputPipeline;
    qDebug() << "  - bitrate:" << bitrate;
    qDebug() << "  - scaling:" << scaling;
    qDebug() << "  - fps:" << fps;

    QString command = QString(
                          R"({"cmd":"createCamera","values":{"name":"%1","url":"%2","ownerId":1,"settings":{"videoBitrate":%3,"scaling":%4,"FPS":%5}, "onvif":{"port":12345,"username":"","password":""}},"cmdId":%6})")
                          .arg(cameraName)
                          .arg(_cameraInputPipeline)
                          .arg(bitrate)
                          .arg(scaling)
                          .arg(fps)
                          .arg(static_cast<int>(CommandID::createCreateCameraCommand));
    qDebug() << "AICompressor: generated createCamera command:" << command;
    return command;
}

QString AICompressor::createModifyCameraCommand(int cameraId, int videoBitrate)
{
    return QString(
               R"({"cmd":"modifyCamera","id":%1,"values":{"settings":{"videoBitrate":%2}},"cmdId":%3})")
        .arg(cameraId)
        .arg(videoBitrate)
        .arg(static_cast<int>(CommandID::createModifyCameraCommand));
}

QString AICompressor::createModifyCameraScaleCommand(int cameraId, int scaling)
{
    return QString(
               R"({"cmd":"modifyCamera","id":%1,"values":{"settings":{"scaling":%2}},"cmdId":%3})")
        .arg(cameraId)
        .arg(scaling)
        .arg(static_cast<int>(CommandID::createModifyCameraScaleCommand));
}

QString AICompressor::createModifyCameraFpsCommand(int cameraId, int fps)
{
    return QString(
               R"({"cmd":"modifyCamera","id":%1,"values":{"settings":{"FPS":%2}},"cmdId":%3})")
        .arg(cameraId)
        .arg(fps)
        .arg(static_cast<int>(CommandID::createModifyCameraFpsCommand));
}

QString AICompressor::createModifyAllAtributeCommand(int cameraId, int videoBitrate, int scaling, int fps)
{
    return QString(
               R"({"cmd":"modifyCamera","id":%1,"values":{"settings":{"videoBitrate":%2,"scaling":%3,"FPS":%4}},"cmdId":%5})")
        .arg(cameraId)
        .arg(videoBitrate)
        .arg(scaling)
        .arg(fps)
        .arg(static_cast<int>(CommandID::createModifyAllAtributeCommand));
}

// ================== Camera Handling ==================

void AICompressor::processGetCamerasResponse(const QString &response)
{
    qDebug() << "AICompressor: processGetCamerasResponse called";
    qDebug() << "AICompressor: response length:" << response.length();
    qDebug() << "AICompressor: response payload (first 300 chars):" << response.left(300);

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(response.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
    {
        qDebug() << "AICompressor: ERROR - JSON parse failed:" << error.errorString();
        qDebug() << "AICompressor: parse error offset:" << error.offset;
        return;
    }

    qDebug() << "AICompressor: JSON parsed successfully";
    QJsonObject obj = doc.object();

    if (obj.contains("reply"))
    {
        qDebug() << "AICompressor: found 'reply' key in response";
        QJsonArray cameras = obj["reply"].toArray();
        qDebug() << "AICompressor: cameras array size:" << cameras.size() << " and "<<AICompressor::isNewSession;

        if (cameras.size() > 0 && AICompressor::isNewSession) {
            AICompressor::isNewSession = false;
            this->deleteAllCameras(cameras);
            cameras = QJsonArray();
            qDebug() << "---> Clear camera: "<<cameras.size();
            return;
        }
        else if (cameras.size() > 0 && !AICompressor::isNewSession)  {
            emit this->onCameraReady();
        }

        for (int i = 0; i < cameras.size(); ++i) {
            QJsonObject camera = cameras[i].toObject();
            int cameraId = camera.value("id").toInt();
            QString cameraName = camera.value("name").toString();
            QString cameraUrl = camera.value("url").toString();
            QString status = camera.value("status").toString();

            qDebug() << "AICompressor: Camera" << i << "details:";
            qDebug() << "  - ID:" << cameraId;
            qDebug() << "  - Name:" << cameraName;
            qDebug() << "  - URL:" << cameraUrl;
            qDebug() << "  - Status:" << status;
            qDebug() << "  - Camera output pipeline: "<<_cameraOutputPipeline;
            this->setCameraStatus(status);
            _cameraId = cameraId;

            if (camera.contains("settings")) {
                QJsonObject settings = camera.value("settings").toObject();
                int bitrate = settings.value("videoBitrate").toInt();
                int fps = settings.value("FPS").toInt();
                int scaling = settings.value("scaling").toInt();

                qDebug() << "  - Settings: bitrate=" << bitrate << "fps=" << fps << "scaling=" << scaling;
            }
        }

        storeCameraData(cameras);

        hasCameras = !cameraNames.isEmpty();
        qDebug() << "AICompressor: hasCameras flag set to:" << hasCameras;
        qDebug() << "AICompressor: total cameras found:" << cameraNames.size();
        qDebug() << "AICompressor: camera names list:" << cameraNames;

        if (!cameraNames.isEmpty())
        {
            selectedCameraName = cameraNames.first();
            qDebug() << "AICompressor: auto-selected first camera:" << selectedCameraName;

            QJsonObject *selectedCamera = findCameraByName(selectedCameraName);
            if (selectedCamera) {
                int selectedCameraId = selectedCamera->value("id").toInt();
                qDebug() << "AICompressor: selected camera ID:" << selectedCameraId;
            }

            emit cameraSelected(selectedCameraName);
            qDebug() << "AICompressor: cameraSelected signal emitted";
        }
        else
        {
            qDebug() << "AICompressor: no cameras found in response";
            qDebug() << "AICompressor: creating default camera 'stream'";
            createNewCamera("xbstream");
        }
    } else {
        qDebug() << "AICompressor: ERROR - response does not contain 'reply' key";
        qDebug() << "AICompressor: available keys:" << obj.keys();
    }
}

void AICompressor::createNewCamera(QString cameraName)
{
    this->setCameraStatus("");
    qDebug() << "AICompressor: createNewCamera called with cameraName:" << cameraName;

    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "AICompressor: ERROR - cannot create camera, websocket not connected. State:" << webSocket->state();
        return;
    }

    qDebug() << "AICompressor: websocket is connected, proceeding to create camera";

    int defaultBitrate = 2000;
    int defaultScaling = 100;
    int defaultFps = 25;

    qDebug() << "AICompressor: using default values - bitrate:" << defaultBitrate << "scaling:" << defaultScaling << "fps:" << defaultFps;

    QString createCameraCommand = createCreateCameraCommand(cameraName, _cameraInputPipeline, _bitrate, _scaling, _fps);
    qDebug() << "AICompressor: sending createCamera command for" << cameraName;
    qDebug() << "AICompressor: command length:" << createCameraCommand.length();

    webSocket->sendTextMessage(createCameraCommand);
    qDebug() << "AICompressor: createCamera command sent successfully";
}

bool AICompressor::isInputConnected()
{
    qDebug() << "--> cam status: AICompressor::isInputConnected: "<<_cameraStatus;
    return _cameraStatus == CAMERA_STATUS_IDLE || _cameraStatus == CAMERA_STATUS_WORKING;
}

void AICompressor::storeCameraData(const QJsonArray &cameras)
{
    qDebug() << "AICompressor: storing" << cameras.size() << "camera entries.";
    cameraList = cameras;
    cameraNames.clear();

    for (const auto &value : cameras)
    {
        QJsonObject camera = value.toObject();
        if (camera.contains("name"))
        {
            QString name = camera["name"].toString();
            cameraNames.append(name);
            qDebug() << "AICompressor: stored camera name:" << name;
        }
    }
}

QJsonObject *AICompressor::findCameraByName(const QString &cameraName)
{
    qDebug() << "AICompressor: searching for camera by name:" << cameraName;
    for (auto it = cameraList.begin(); it != cameraList.end(); ++it)
    {
        QJsonObject camera = it->toObject();
        if (camera.contains("name") && camera["name"].toString() == cameraName)
        {
            static QJsonObject result;
            result = camera;
            qDebug() << "AICompressor: camera found, id =" << camera.value("id").toInt();
            return &result;
        }
    }
    qDebug() << "AICompressor: camera not found:" << cameraName;
    return nullptr;
}

// ================== Update Camera Params ==================

void AICompressor::updateCameraBitrate(const QString &cameraName, int newBitrate)
{
    QJsonObject *camera = findCameraByName(cameraName);
    if (!camera)
        return;

    int cameraId = camera->value("id").toInt();
    QString modifyCommand = createModifyCameraCommand(cameraId, newBitrate);
    updateCameraParameter(cameraName, "videoBitrate", newBitrate, modifyCommand);
    emit bitrateUpdated(cameraName, newBitrate);
}

void AICompressor::updateCameraScale(const QString &cameraName, int scaling)
{
    QJsonObject *camera = findCameraByName(cameraName);
    if (!camera)
        return;

    int cameraId = camera->value("id").toInt();
    QString modifyCommand = createModifyCameraScaleCommand(cameraId, scaling);
    updateCameraParameter(cameraName, "scaling", scaling, modifyCommand);
    emit scaleUpdated(cameraName, scaling);
}

void AICompressor::updateCameraFps(const QString &cameraName, int newFps)
{
    QJsonObject *camera = findCameraByName(cameraName);
    if (!camera)
        return;

    int cameraId = camera->value("id").toInt();
    QString modifyCommand = createModifyCameraFpsCommand(cameraId, newFps);
    updateCameraParameter(cameraName, "FPS", newFps, modifyCommand);
    emit fpsUpdated(cameraName, newFps);
}

void AICompressor::deleteCamera()
{
    QString command = deleteCameraCommand(_cameraId);
    qDebug() << "--> AICompressor::deleteCamera: "<<_cameraId << ": "<<command;
    this->sendCommand(command);
    this->setCameraStatus("");
}

QString AICompressor::cameraStatus() const
{
    return _cameraStatus;
}

void AICompressor::setCameraStatus(const QString &newCameraStatus)
{
    if (_cameraStatus == newCameraStatus)
        return;
    _cameraStatus = newCameraStatus;
}

int AICompressor::bitrate() const
{
    return _bitrate;
}

void AICompressor::setBitrate(int newBitrate)
{
    _bitrate = newBitrate;
}

QString AICompressor::getState()
{
    return _cameraStatus;
}

void AICompressor::updateCameraParameter(const QString &cameraName, const QString &paramName, int value, const QString &modifyCommand)
{
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "AICompressor: cannot update camera parameter, websocket not connected.";
        return;
    }

    qDebug() << "AICompressor: sending modify command for camera" << cameraName << "param" << paramName << "value" << value;
    webSocket->sendTextMessage(modifyCommand);

    QJsonObject *camera = findCameraByName(cameraName);
    if (!camera) {
        qDebug() << "AICompressor: update aborted, camera not found locally:" << cameraName;
        return;
    }

    int cameraId = camera->value("id").toInt();
    QJsonObject settings = camera->value("settings").toObject();
    settings[paramName] = value;

    QJsonObject updatedCamera = *camera;
    updatedCamera["settings"] = settings;

    for (auto it = cameraList.begin(); it != cameraList.end(); ++it)
    {
        QJsonObject cam = it->toObject();
        if (cam.value("id").toInt() == cameraId)
        {
            *it = updatedCamera;
            qDebug() << "AICompressor: updated local cache for camera id" << cameraId;
            break;
        }
    }
}

// ================== Helpers ==================

void AICompressor::closeConnection()
{
    qDebug() << "AICompressor: closing WebSocket connection by request.";
    _shouldReconnect = false;  // THÊM: Disable reconnect when closing manually
    
    // THÊM: Stop timer
    if (_reconnectTimer && _reconnectTimer->isActive()) {
        _reconnectTimer->stop();
    }
    
    if (webSocket && webSocket->state() == QAbstractSocket::ConnectedState)
        webSocket->close();
}

void AICompressor::updateBitrate(int newBitrate)
{
    _bitrate = newBitrate;
    if (!selectedCameraName.isEmpty())
        updateCameraBitrate(selectedCameraName, newBitrate);
}

void AICompressor::updateScale(int scaling)
{
    if (!selectedCameraName.isEmpty())
        updateCameraScale(selectedCameraName, scaling);
}

void AICompressor::updateFps(int newFps)
{
    if (!selectedCameraName.isEmpty())
        updateCameraFps(selectedCameraName, newFps);
}

void AICompressor::deleteAllCameras(QJsonArray cameras)
{
    qDebug() << "AICompressor: deleteAllCameras"<<cameras.size();
    for (int i = 0; i < cameras.size(); ++i) {
        QJsonObject camera = cameras[i].toObject();
        int cameraId = camera.value("id").toInt();
        QString command = this->deleteCameraCommand(cameraId);
        qDebug() << "--> delete camera: "<<cameraId<<"-: "<<command;
        this->sendCommand(command);
    }
}

void AICompressor::refreshCamera()
{
    qDebug() << "--> Do AICompressor::refreshCamera: "<<QString("{\"cmd\": \"refreshCamera\", \"id\": %2, \"cmdId\": %1}").arg(QString::number(static_cast<int>(CommandID::refreshCamera)))
                                 .arg(QString::number(_cameraId));
    this->sendCommand(QString("{\"cmd\": \"refreshCamera\", \"id\": %2, \"cmdId\": %1}").arg(QString::number(static_cast<int>(CommandID::refreshCamera)))
                          .arg(QString::number(_cameraId)));
}

QString AICompressor::getSelectedCamera() const
{
    return selectedCameraName;
}

void AICompressor::scheduleReconnect(int delayMs)
{
    if (!_shouldReconnect) {
        qDebug() << "AICompressor: reconnect disabled, not scheduling";
        return;
    }
    
    if (_isReconnecting) {
        qDebug() << "AICompressor: reconnect already scheduled";
        return;
    }
    
    _isReconnecting = true;
    qWarning() << "AICompressor: scheduling reconnect in" << delayMs << "ms";
    _reconnectTimer->start(delayMs);
}

void AICompressor::attemptReconnect()
{
    if (!_shouldReconnect) {
        qDebug() << "AICompressor: reconnect cancelled (stop was called)";
        _isReconnecting = false;
        return;
    }
    
    qWarning() << "AICompressor: attempting reconnect...";
    qWarning() << "AICompressor: previous state:" << webSocket->state();
    
    // Close existing connection if any
    if (webSocket->state() != QAbstractSocket::UnconnectedState) {
        qDebug() << "AICompressor: closing existing connection";
        webSocket->abort();
    }
    
    // Reset state
    this->setCameraStatus("");
    cameraNames.clear();
    cameraList = QJsonArray();
    selectedCameraName.clear();
    hasCameras = false;
    
    // Start fresh
    AICompressor::isNewSession = true;
    _isReconnecting = false;
    
    qWarning() << "AICompressor: initiating new connection to" << serverUrl;
    this->connectToServer();
}
