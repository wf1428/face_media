/**
 * @file ui_message_box.h
 * @brief 在 Wayland 下使用应用内覆盖层显示消息框，并保留其他平台原行为。
 */

#ifndef UI_MESSAGE_BOX_H
#define UI_MESSAGE_BOX_H

#include <QMessageBox>
#include <QString>

class QWidget;

namespace UiMessageBox {

QMessageBox::StandardButton information(
        QWidget *parent,
        const QString &title,
        const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton warning(
        QWidget *parent,
        const QString &title,
        const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton critical(
        QWidget *parent,
        const QString &title,
        const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton question(
        QWidget *parent,
        const QString &title,
        const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

} // namespace UiMessageBox

#endif // UI_MESSAGE_BOX_H
