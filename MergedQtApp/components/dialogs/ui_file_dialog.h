/**
 * @file ui_file_dialog.h
 * @brief 提供 Wayland kiosk 兼容的应用内目录选择框。
 */

#ifndef UI_FILE_DIALOG_H
#define UI_FILE_DIALOG_H

#include <QFileDialog>
#include <QString>

class QWidget;

namespace UiFileDialog {

QString getExistingDirectory(
        QWidget *parent,
        const QString &caption,
        const QString &directory = QString(),
        QFileDialog::Options options = QFileDialog::ShowDirsOnly);

} // namespace UiFileDialog

#endif // UI_FILE_DIALOG_H
