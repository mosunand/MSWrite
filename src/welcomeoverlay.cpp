// welcomeoverlay.cpp — see welcomeoverlay.h.

#include "welcomeoverlay.h"

#include "uidialogs.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <initializer_list>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QDate>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtMath>
#include <QRandomGenerator>

namespace {
constexpr int kMottoBudget = 19;     // 字符宽预算(汉字 1、两个半角 1)
constexpr int kCelebrateFrames = 80; // 33ms/帧 ≈ 2.6 秒

const char *kFlakeColors[] = {
    "#5b8def", "#f5a623", "#2ecc71", "#e74c3c",
    "#9b59b6", "#1abc9c", "#f1c40f", "#4aa3ff"
};
}

WelcomeOverlay::WelcomeOverlay(QWidget *host)
    : QDialog(host, Qt::Window | Qt::FramelessWindowHint)
{
    setWindowModality(Qt::ApplicationModal);
    // 注意:本对象由宿主栈上分配(showWelcome 里),绝不能 WA_DeleteOnClose,
    // 否则 accept 后 Qt 先删一次、栈析构再删一次 = 双重删除崩溃
    const bool dark = QSettings().value(QStringLiteral("theme"),
        QSettings().value(QStringLiteral("theme"), QStringLiteral("light")).toString())
        == QLatin1String("dark");
    // 与主窗口同大同位:盖住整个主界面
    setGeometry(host->frameGeometry());
    setStyleSheet(QStringLiteral(R"(
QDialog { background:%1; }
QLabel { color:%2; background:transparent; }
QLabel#muted { color:%3; }
QLabel#quote { color:%3; font-size:15px; font-style:italic; }
QLabel#cheer { font-size:40px; font-weight:700; }
QLineEdit { background:%4; color:%2; border:1px solid %5; border-radius:8px;
            padding:11px 14px; font-size:14px; selection-background-color:#3b67ce; }
QLineEdit:focus { border-color:#6784d7; }
QPushButton { border-radius:8px; padding:11px 26px; font-size:14px; }
QPushButton#ghost { background:transparent; color:%3; border:1px solid %5; }
QPushButton#ghost:hover { color:%2; border-color:#6784d7; }
QPushButton#primary { background:#315ec7; border:1px solid #2a51ad; color:white; font-weight:600; }
QPushButton#primary:hover { background:#2a51ad; }
)")
        .arg(dark ? "#141519" : "#f6f7fa",
             dark ? "#eceef2" : "#202631",
             dark ? "#a1aab9" : "#687386",
             dark ? "#20232b" : "#ffffff",
             dark ? "#343945" : "#e0e4ec"));
    buildUi();
    // 主窗口几何就绪后再贴齐(首启时宿主可能还没完成布局)
    QTimer::singleShot(0, this, [this, host] { setGeometry(host->frameGeometry()); });
    m_animTimer.setInterval(33);
    connect(&m_animTimer, &QTimer::timeout, this, [this] {
        for (Flake &f : m_flakes) {
            f.phase += 0.09;
            f.vy += f.gravity;                  // 上抛弹受重力回落
            f.pos.rx() += f.vx + qSin(f.phase) * 0.7;
            f.pos.ry() += f.vy;
            f.rot += f.vrot;
        }
        if (--m_framesLeft <= 0)
            accept();   // 礼花放完,正式进入软件
        update();
    });
}

void WelcomeOverlay::buildUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(48, 64, 48, 56);
    lay->addStretch(2);

    m_logo = new QLabel(this);
    m_logo->setObjectName(QStringLiteral("muted"));
    QPixmap icon(QStringLiteral(":/logo.ico"));
    if (!icon.isNull())
        m_logo->setPixmap(icon.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_logo->setAlignment(Qt::AlignHCenter);
    lay->addWidget(m_logo);

    m_title = new QLabel(tr("Mswrite"), this);
    m_title->setAlignment(Qt::AlignHCenter);
    m_title->setStyleSheet(QStringLiteral("font-size:34px;font-weight:700;"));
    lay->addSpacing(6);
    lay->addWidget(m_title);

    m_version = new QLabel(tr("版本 %1").arg(QApplication::applicationVersion()), this);
    m_version->setObjectName(QStringLiteral("muted"));
    m_version->setAlignment(Qt::AlignHCenter);
    lay->addWidget(m_version);

    m_slogan = new QLabel(tr("所见即所得 · 写作、阅读与 AI,在同一个工作空间"), this);
    m_slogan->setObjectName(QStringLiteral("muted"));
    m_slogan->setAlignment(Qt::AlignHCenter);
    lay->addSpacing(10);
    lay->addWidget(m_slogan);

    // 记笔记的名人名言:一句中文(国人)、一句英文(外国)
    m_quoteZh = new QLabel(tr("「不动笔墨不读书。」—— 徐特立"), this);
    m_quoteZh->setObjectName(QStringLiteral("quote"));
    m_quoteZh->setAlignment(Qt::AlignHCenter);
    lay->addSpacing(14);
    lay->addWidget(m_quoteZh);
    m_quoteEn = new QLabel(QStringLiteral("“Reading maketh a full man, conference a ready man, "
                                              "and writing an exact man.” — Francis Bacon"), this);
    m_quoteEn->setObjectName(QStringLiteral("quote"));
    m_quoteEn->setAlignment(Qt::AlignHCenter);
    lay->addSpacing(6);
    lay->addWidget(m_quoteEn);

    lay->addStretch(2);

    m_motto = new QLineEdit(this);
    m_motto->setValidator(UiDialogs::makeMottoValidator(this));
    m_motto->setPlaceholderText(tr("写一句座右铭,它会留在状态栏(可跳过,最多 19 个字符宽)"));
    m_motto->setAlignment(Qt::AlignHCenter);
    m_motto->setFixedWidth(460);
    m_motto->setText(QSettings().value(QStringLiteral("motto")).toString());
    lay->addWidget(m_motto, 0, Qt::AlignHCenter);
    connect(m_motto, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (UiDialogs::mottoWidth(text) > kMottoBudget)
            m_hint->setText(tr("再减一点点就到 19 了"));
        else
            m_hint->setText(QString());
    });

    m_hint = new QLabel(QString(), this);
    m_hint->setObjectName(QStringLiteral("muted"));
    m_hint->setAlignment(Qt::AlignHCenter);
    lay->addSpacing(4);
    lay->addWidget(m_hint);

    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    m_skip = new QPushButton(tr("跳过"), this);
    m_skip->setObjectName(QStringLiteral("ghost"));
    m_enter = new QPushButton(tr("进入软件"), this);
    m_enter->setObjectName(QStringLiteral("primary"));
    m_enter->setDefault(true);
    row->addStretch(1);
    row->addWidget(m_skip);
    row->addWidget(m_enter);
    row->addStretch(1);
    lay->addSpacing(8);
    lay->addLayout(row);

    lay->addStretch(3);

    m_cheer = new QLabel(this);
    m_cheer->setObjectName(QStringLiteral("cheer"));
    m_cheer->setAlignment(Qt::AlignHCenter);
    m_cheer->hide();
    lay->insertWidget(0, m_cheer);
    lay->insertStretch(1, 1);

    connect(m_skip, &QPushButton::clicked, this, [this] {
        m_motto->clear();
        startCelebration();
    });
    connect(m_enter, &QPushButton::clicked, this, [this] { startCelebration(); });
}

void WelcomeOverlay::startCelebration()
{
    if (m_celebrating)
        return;
    m_celebrating = true;
    // 落库:座右铭(可空)+ 不再显示欢迎页 + 陪伴起点(首次完成欢迎仪式的日期)
    QSettings().setValue(QStringLiteral("motto"), m_motto->text().trimmed());
    QSettings().setValue(QStringLiteral("welcomeDone"), true);
    if (!QSettings().contains(QStringLiteral("welcomeFirstAt")))
        QSettings().setValue(QStringLiteral("welcomeFirstAt"),
                             QDate::currentDate().toString(Qt::ISODate));
    // 切到庆祝形态:欢迎词 + 礼花。欢迎词是固定问候,不拼接座右铭
    // (座右铭属于状态栏,混进问候语很怪 —— 用户反馈)
    for (QWidget *w : std::initializer_list<QWidget *>{ m_logo, m_title, m_version, m_slogan,
                        m_quoteZh, m_quoteEn, m_motto, m_hint, m_skip, m_enter })
        if (w) w->hide();
    m_cheer->setText(tr("欢迎使用 Mswrite,祝你落笔生花"));
    m_cheer->show();
    spawnFlakes();
    m_framesLeft = kCelebrateFrames;
    m_animTimer.start();
}

void WelcomeOverlay::spawnFlakes()
{
    // 大礼花:数量翻倍、纸屑加大,另有一波从底部两角向上喷发后受重力回落
    m_flakes.clear();
    m_flakes.reserve(260);
    for (int i = 0; i < 170; ++i) {   // 天上飘落的大纸屑
        Flake f;
        f.pos = QPointF(qBound(0.0, width() * (0.05 + 0.9 * (qreal(i) / 170.0))
                                   + (int(QRandomGenerator::global()->bounded(70)) - 35), qreal(width())),
                        qreal(-60 - int(QRandomGenerator::global()->bounded(int(height() * 0.7)))));
        f.vy = 3.2 + (int(QRandomGenerator::global()->bounded(40))) / 10.0;
        f.gravity = 0.0;
        f.phase = int(QRandomGenerator::global()->bounded(628)) / 100.0;
        f.rot = int(QRandomGenerator::global()->bounded(360));
        f.vrot = (int(QRandomGenerator::global()->bounded(15)) - 7) / 2.0;
        f.size = 10 + int(QRandomGenerator::global()->bounded(12));
        f.color = QColor(kFlakeColors[int(QRandomGenerator::global()->bounded(8))]);
        m_flakes.append(f);
    }
    for (int i = 0; i < 90; ++i) {   // 底部两角向上喷的礼花弹
        const bool left = i % 2 == 0;
        Flake f;
        f.pos = QPointF(left ? qreal(width() * 0.06) : qreal(width() * 0.94),
                        qreal(height() * 0.92));
        const double angle = (left ? -0.5 : -2.64) + (int(QRandomGenerator::global()->bounded(50)) - 25) / 100.0;
        const double speed = 14 + int(QRandomGenerator::global()->bounded(14));
        f.vx = qCos(angle) * speed;
        f.vy = qSin(angle) * speed;
        f.gravity = 0.34;
        f.phase = int(QRandomGenerator::global()->bounded(628)) / 100.0;
        f.rot = int(QRandomGenerator::global()->bounded(360));
        f.vrot = (int(QRandomGenerator::global()->bounded(20)) - 10) / 2.0;
        f.size = 9 + int(QRandomGenerator::global()->bounded(11));
        f.color = QColor(kFlakeColors[int(QRandomGenerator::global()->bounded(8))]);
        m_flakes.append(f);
    }
}

void WelcomeOverlay::paintEvent(QPaintEvent *event)
{
    QDialog::paintEvent(event);
    if (!m_celebrating)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (const Flake &f : m_flakes) {
        p.save();
        p.translate(f.pos);
        p.rotate(f.rot);
        p.setPen(Qt::NoPen);
        p.setBrush(f.color);
        p.drawRoundedRect(QRectF(-f.size / 2.0, -f.size / 4.0, f.size, f.size / 2.0), 1.5, 1.5);
        p.restore();
    }
}

void WelcomeOverlay::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_celebrating)
        spawnFlakes();
}

void WelcomeOverlay::keyPressEvent(QKeyEvent *event)
{
    // 欢迎页必须走按钮:屏蔽 Esc,回车交给 default 按钮(进入软件)
    if (event->key() == Qt::Key_Escape)
        return;
    QDialog::keyPressEvent(event);
}
