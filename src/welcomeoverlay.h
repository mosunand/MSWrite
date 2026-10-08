#pragma once
// welcomeoverlay.h — 首次启动欢迎页。覆盖在主窗口上(与主窗口同大),展示
// Logo/版本号,引导输入座右铭(可跳过);点击「进入软件」后放礼花、停留
// 数秒再进入,之后不再出现(welcomeDone 落 QSettings)。座右铭写入 QSettings
// 的 "motto",留空 = 状态栏显示默认的「所见即所得」;可在偏好设置改/取消。

#include <QDialog>
#include <QColor>
#include <QPainterPath>
#include <QTimer>
#include <QVector>

class QLineEdit;
class QLabel;
class QPushButton;
class QWidget;

class WelcomeOverlay : public QDialog {
    Q_OBJECT
public:
    explicit WelcomeOverlay(QWidget *host);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;   // 屏蔽 Esc/回车默认关闭

private:
    void buildUi();
    void startCelebration();
    void spawnFlakes();

    QWidget *m_formHost = nullptr;      // Logo/版本/座右铭/按钮
    QLabel *m_logo = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_version = nullptr;
    QLabel *m_slogan = nullptr;
    QLabel *m_quoteZh = nullptr;        // 记笔记名言(中文)
    QLabel *m_quoteEn = nullptr;        // 记笔记名言(英文)
    QLineEdit *m_motto = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_skip = nullptr;
    QPushButton *m_enter = nullptr;

    QLabel *m_cheer = nullptr;          // 庆祝阶段的大欢迎词
    bool m_celebrating = false;
    QTimer m_animTimer;
    int m_framesLeft = 0;
    struct Flake {
        QPointF pos;
        double vx = 0;
        double vy = 0;
        double gravity = 0;             // 上抛弹 > 0:受重力回落
        double phase = 0;
        double rot = 0;
        double vrot = 0;
        int size = 0;
        QColor color;
    };
    QVector<Flake> m_flakes;
};
