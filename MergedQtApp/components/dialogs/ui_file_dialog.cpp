/**
 * @file ui_file_dialog.cpp
 * @brief 实现固定在现有 Wayland surface 内的全屏目录选择器。
 */

#include "ui_file_dialog.h"

#include "common/responsive_ui.h"

#include <QEvent>
#include <QShowEvent>
#include <QStringList>

namespace {

class EmbeddedDirectoryDialog final : public QFileDialog
{
public:
    EmbeddedDirectoryDialog(QWidget *parent,
                            const QString &caption,
                            const QString &directory,
                            QFileDialog::Options options)
        : QFileDialog(ResponsiveUi::overlayHost(parent), caption, directory)
    {
        setOptions(options | QFileDialog::DontUseNativeDialog |
                   QFileDialog::ShowDirsOnly);
        setFileMode(QFileDialog::Directory);
        setAcceptMode(QFileDialog::AcceptOpen);
        setWindowFlags(Qt::Widget);
        setModal(true);
        setAttribute(Qt::WA_StyledBackground, true);
        if (parentWidget()) {
            parentWidget()->installEventFilter(this);
            setGeometry(parentWidget()->rect());
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return QFileDialog::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        if (parentWidget()) {
            setGeometry(parentWidget()->rect());
        }
        QFileDialog::showEvent(event);
        raise();
    }
};

} // namespace

namespace UiFileDialog {

QString getExistingDirectory(QWidget *parent,
                             const QString &caption,
                             const QString &directory,
                             QFileDialog::Options options)
{
    if (!ResponsiveUi::isWaylandPlatform()) {
        return QFileDialog::getExistingDirectory(parent, caption, directory, options);
    }

    EmbeddedDirectoryDialog dialog(parent, caption, directory, options);
    if (dialog.exec() != QDialog::Accepted) {
        return QString();
    }
    const QStringList selected = dialog.selectedFiles();
    return selected.isEmpty() ? QString() : selected.first();
}

} // namespace UiFileDialog
