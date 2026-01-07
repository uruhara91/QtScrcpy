#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QPointer>
#include <QWidget>
#include <span>
#include <atomic>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYuvOpenGLWidget;
class QLabel;

class VideoForm : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, QWidget *parent = Q_NULLPTR);
    ~VideoForm();

    void staysOnTop(bool top = true);
    void updateShowSize(const QSize &newSize);
    void setSerial(const QString& serial);
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

    void updateRender(int width, int height, 
                      std::span<const uint8_t> dataY, 
                      std::span<const uint8_t> dataU, 
                      std::span<const uint8_t> dataV, 
                      int linesizeY, int linesizeU, int linesizeV);

    const QSize &frameSize();

private:
    // DeviceObserver implementation
    void onFrame(int width, int height, 
                 std::span<const uint8_t> dataY, 
                 std::span<const uint8_t> dataU, 
                 std::span<const uint8_t> dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);
    
    void initUI();
    void showToolForm(bool show = true);
    void moveCenter();
    void installShortcut();
    QRect getScreenRect();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // UI Pointers
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYuvOpenGLWidget> m_videoWidget;
    QPointer<QLabel> m_fpsLabel;

    // --- State Members ---
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;

    bool show_toolbar = true; 
    bool m_isFullScreen = false;
    bool m_framelessWindow = false;

    std::atomic<bool> m_resizePending = false;
};

#endif // VIDEOFORM_H