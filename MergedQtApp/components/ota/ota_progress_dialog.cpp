/**
 * @file ota_progress_dialog.cpp
 * @brief GUI 线程显示 OTA 进度、成功和失败回退结果，不记录中间日志。
 * @author Du lin
 * @date 2026-11-18
 */
#include "ota_progress_dialog.h"
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QPointer>
#include <QTimer>

OtaProgressDialog::OtaProgressDialog() : QDialog(nullptr, Qt::Dialog | Qt::WindowStaysOnTopHint)
{
    setWindowTitle(QStringLiteral("系统升级"));
    setWindowFlag(Qt::WindowCloseButtonHint, false);
    setMinimumWidth(460);
    setModal(false);
    auto *layout = new QVBoxLayout(this);
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    close_ = new QPushButton(QStringLiteral("关闭"), this);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(message_); layout->addWidget(progress_); layout->addWidget(close_);
}

void OtaProgressDialog::present(const QString &text, int percent, bool terminal, bool success)
{
    static QPointer<OtaProgressDialog> dialog;
    if (!dialog) dialog = new OtaProgressDialog();
    dialog->terminal_ = terminal;
    const auto generation = ++dialog->generation_;
    dialog->message_->setText(text);
    dialog->progress_->setValue(qBound(0, percent, 100));
    dialog->close_->setVisible(terminal);
    dialog->show(); dialog->raise();
    if (terminal && success) {
        const QPointer<OtaProgressDialog> alive = dialog;
        QTimer::singleShot(10000, dialog, [alive, generation] {
            if (alive && alive->generation_ == generation) alive->hide();
        });
    }
}

void OtaProgressDialog::reject()
{
    if (terminal_) QDialog::reject();
}
