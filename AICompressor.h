#ifndef AICOMPRESSOR_H
#define AICOMPRESSOR_H


#include <QObject>
#include <QCoreApplication>
#include <QWebSocket>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#ifdef XBFIRM
#include "Settings.h"
#else
#include "ABRConfigs.h"
#endif
// #include "../settings/Settings.h"


// #define AI_COMPRESSOR_HOST "192.168.1.130"
#define AI_COMPRESSOR_ADMIN "admin"
#define AI_COMPRESSOR_PASSWORD "admin"
// #define AI_COMPRESSOR_PORT 8001

// Camera Status constants
#define CAMERA_STATUS_IDLE "Idle"
#define CAMERA_STATUS_WORKING "Working"
#define CAMERA_STATUS_ERROR "Error"

#define REPLY_OBJECT_ID_STATUS 405

// Định nghĩa enum CommandID (dùng cho switch-case)
enum class CommandID {
    createLoginCommand = 1,
    createGetCamerasCommand,
    createCreateCameraCommand,
    createModifyCameraCommand,
    createModifyCameraScaleCommand,
    createModifyCameraFpsCommand,
    createModifyAllAtributeCommand,
    deleteCameraCommand,
    refreshCamera
};

class AICompressor : public QObject
{
    Q_OBJECT
public:
    explicit AICompressor(QObject *parent = nullptr);
    ~AICompressor();
    static AICompressor* instance();
    void start(QString pipeline);
    void stop();
    QString getOutputRtspURl();

    void connectToServer();
    void closeConnection();
    void getCameras();
    void createNewCamera(QString cameraName);
    void sendCommand(QString command);

    bool isInputConnected();

    void updateBitrate(int newBitrate);
    void updateScale(int scaling);
    void updateFps(int newFps);
    void deleteAllCameras(QJsonArray cameras);
    void refreshCamera();

    QString getSelectedCamera() const;
    // void startAICompressorService();
    // void stopAICompressorService();

    int bitrate() const;
    void setBitrate(int newBitrate);
    QString getState();

    QString cameraStatus() const;
    void setCameraStatus(const QString &newCameraStatus);

signals:
    void sendMessage(const QString &message);
    void cameraSelected(const QString &cameraName);
    void bitrateUpdated(const QString &cameraName, int newBitrate);
    void scaleUpdated(const QString &cameraName, int newScale);
    void fpsUpdated(const QString &cameraName, int newFps);
    void cameraStatusChanged(const QString &cameraName, const QString &status);
    // void aiEncodingServiceStarted(bool success);
    // void aiEncodingServiceStopped(bool success);
    void onOutputStat(int byteIn, int byteOut);
    void onCameraReady();
public slots:
    void handleChangeBitrate(int bitrate);
    void handleChangeFps(int fps);
    void handleChangeScale(int scale);
    void handleGetInfoTimer();
private slots:
    void onConnected();
    void onMessageReceived(const QString &message);
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);  // Thêm dòng này
    void scheduleReconnect(int delayMs);      // THÊM
    void attemptReconnect();                   // THÊM

private:
    static AICompressor* _instance;

private:
    // ==== Command tạo JSON ====
    QString createLoginCommand(QString &username, QString &password);
    QString createGetCamerasCommand();
    QString deleteCameraCommand(int cameraId);
    QString createCreateCameraCommand(const QString &cameraName,
                                      const QString &url = "rtsp://192.168.144.240:8554/payload",
                                      int bitrate = 1000,
                                      int scaling = 80,
                                      int fps = 30);
    QString createModifyCameraCommand(int cameraId, int videoBitrate);
    QString createModifyCameraScaleCommand(int cameraId, int scaling);
    QString createModifyCameraFpsCommand(int cameraId, int fps);
    QString createModifyAllAtributeCommand(int cameraId, int videoBitrate, int scaling, int fps);

    // ==== Xử lý dữ liệu ====
    void processGetCamerasResponse(const QString &response);
    void storeCameraData(const QJsonArray &cameras);
    QJsonObject findCameraByName(const QString &cameraName);
    void updateCameraBitrate(const QString &cameraName, int newBitrate);
    void updateCameraScale(const QString &cameraName, int scaling);
    void updateCameraFps(const QString &cameraName, int newFps);
    void updateCameraParameter(const QString &cameraName, const QString &paramName, int value, const QString &modifyCommand);
    void deleteCamera();

private:
    QWebSocket *webSocket;
    int cnt=0;
    QString serverUrl;
    QString userName;
    QString password;

    QJsonArray cameraList;

    QStringList cameraNames;
    QString selectedCameraName;

    int cmdIDCounter;
    bool hasCameras;
    QString _cameraInputPipeline;
    QString _cameraOutputPipeline;
    int _bitrate;
    int _fps;
    int _scaling;
    QString _cameraStatus;
    QTimer _getInfoTimer;
    int _cameraId;

#ifdef XBFIRM
    GeneralSetting* _generalSettings;
#endif
    static bool isNewSession;
    
    // THÊM 3 dòng này:
    QTimer* _reconnectTimer;
    bool _isReconnecting;
    bool _shouldReconnect;
};

#endif // AICOMPRESSOR_H
