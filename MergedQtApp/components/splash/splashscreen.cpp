/**
 * @file splashscreen.cpp
 * @brief 应用启动期间显示并按定时器自动结束的启动页的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "splashscreen.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QFont>
#include <QIcon>
#include <QApplication>
#include <QDesktopWidget>
#include <QScreen>
#include <QGuiApplication>
#include <QDebug>

/** @brief 创建启动页 UI 并启动结束定时器。 */
SplashScreen::SplashScreen(QWidget *parent)
    : QWidget(parent)
{
    qInfo() << "[SPLASH] ctor enter";

    setupUI();

    // EGLFS 只能保留一个原生顶层窗口。存在父窗口时，启动页必须是普通子控件，
    // 由唯一的 EGLFS 根窗口负责最终合成；仅桌面调试且没有父窗口时才保留顶层行为。
    if (parentWidget()) {
        setWindowFlags(Qt::Widget);
        setGeometry(parentWidget()->rect());
    } else {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            setGeometry(screen->geometry());
        } else {
            showFullScreen();
        }
#else
        setGeometry(QApplication::desktop()->screen()->geometry());
#endif
    }

    // 白底
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("background-color: white;");

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SplashScreen::onTimerTimeout);
    m_timer->start(3500);

    qInfo() << "[SPLASH] timer started 3500ms";
}

/** @brief 停止并释放定时器。 */
SplashScreen::~SplashScreen()
{
    qInfo() << "[SPLASH] dtor";
}

/** @brief 创建背景、图标或标题布局。 */
void SplashScreen::setupUI()
{
    qInfo() << "[SPLASH] setupUI";

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);
    layout->setAlignment(Qt::AlignCenter);

    QLabel *imageLabel = new QLabel(this);
    imageLabel->setAlignment(Qt::AlignCenter);

    QPixmap pixmap(":/static/images/show_company_image.png");
    if (!pixmap.isNull()) {
        imageLabel->setPixmap(pixmap.scaled(220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        imageLabel->setText("Logo");
        imageLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: #333333;");
    }

    QLabel *textLabel = new QLabel(QStringLiteral("微远智控"), this);
    textLabel->setAlignment(Qt::AlignCenter);

    QFont font = textLabel->font();
    font.setPointSize(24);
    font.setBold(true);
    textLabel->setFont(font);
    textLabel->setStyleSheet("color: #333333;");

    layout->addStretch();
    layout->addWidget(imageLabel, 0, Qt::AlignCenter);
    layout->addWidget(textLabel, 0, Qt::AlignCenter);
    layout->addStretch();

    setLayout(layout);
    setWindowIcon(QIcon(":/static/images/show_company_image.png"));
}

/** @brief 停止定时器并通知主界面显示。 */
void SplashScreen::onTimerTimeout()
{
    qInfo() << "[SPLASH] timeout emit splashFinished";
    emit splashFinished();
}
