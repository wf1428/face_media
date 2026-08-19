#ifndef ACCESS_PASSWORD_DIALOG_H
#define ACCESS_PASSWORD_DIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QQuickWidget;
class QShowEvent;

/** @brief 人脸识别主页使用的人员通行密码输入窗口。 */
class AccessPasswordDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AccessPasswordDialog(QWidget *parent = nullptr);
    ~AccessPasswordDialog() override;

    QString password() const;
    static bool getPassword(QWidget *parent, QString *password);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void acceptInput();
    void showKeyboard();
    void hideKeyboard();

    QLineEdit *passwordEdit_ = nullptr;
    QQuickWidget *keyboardWidget_ = nullptr;
};

#endif // ACCESS_PASSWORD_DIALOG_H
