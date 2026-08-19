/********************************************************************************
** Form generated from reading UI file 'networkpage.ui'
**
** Created by: Qt User Interface Compiler version 5.12.8
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_NETWORKPAGE_H
#define UI_NETWORKPAGE_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_NetworkPage
{
public:
    QVBoxLayout *verticalLayout;
    QLabel *titleLabel;
    QGroupBox *connectionTypeGroup;
    QHBoxLayout *horizontalLayout;
    QLabel *label;
    QComboBox *connectionTypeCombo;
    QSpacerItem *horizontalSpacer;
    QStackedWidget *networkStack;
    QWidget *page;
    QGroupBox *ipSettingsGroup;
    QGridLayout *gridLayout;
    QLabel *label_3;
    QLineEdit *gatewayEdit;
    QLabel *label_2;
    QLabel *label_4;
    QLineEdit *ipAddressEdit;
    QLineEdit *subnetMaskEdit;
    QLineEdit *dnsEdit;
    QLabel *label_5;
    QComboBox *ipModeComboBox;
    QWidget *page_2;
    QWidget *mainCard;
    QVBoxLayout *verticalLayout_2;
    QHBoxLayout *horizontalLayout_2;
    QSpacerItem *horizontalSpacer_2;
    QPushButton *cancelButton;
    QPushButton *applyButton;

    void setupUi(QWidget *NetworkPage)
    {
        if (NetworkPage->objectName().isEmpty())
            NetworkPage->setObjectName(QString::fromUtf8("NetworkPage"));
        NetworkPage->resize(600, 516);
        verticalLayout = new QVBoxLayout(NetworkPage);
        verticalLayout->setSpacing(20);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        verticalLayout->setContentsMargins(30, 30, 30, 30);
        titleLabel = new QLabel(NetworkPage);
        titleLabel->setObjectName(QString::fromUtf8("titleLabel"));
        titleLabel->setStyleSheet(QString::fromUtf8("font-size: 24px; font-weight: bold; color: #333333;"));

        verticalLayout->addWidget(titleLabel);

        connectionTypeGroup = new QGroupBox(NetworkPage);
        connectionTypeGroup->setObjectName(QString::fromUtf8("connectionTypeGroup"));
        connectionTypeGroup->setStyleSheet(QString::fromUtf8("QGroupBox {\n"
"    font-size: 16px;\n"
"    font-weight: bold;\n"
"    color: #333333;\n"
"    border: none;\n"
"    margin-bottom: 10px;\n"
"}\n"
""));
        horizontalLayout = new QHBoxLayout(connectionTypeGroup);
        horizontalLayout->setObjectName(QString::fromUtf8("horizontalLayout"));
        label = new QLabel(connectionTypeGroup);
        label->setObjectName(QString::fromUtf8("label"));
        label->setStyleSheet(QString::fromUtf8("font-size: 14px;\n"
"color: #666666;"));

        horizontalLayout->addWidget(label);

        connectionTypeCombo = new QComboBox(connectionTypeGroup);
        connectionTypeCombo->addItem(QString());
        connectionTypeCombo->addItem(QString());
        connectionTypeCombo->setObjectName(QString::fromUtf8("connectionTypeCombo"));
        connectionTypeCombo->setMinimumSize(QSize(200, 36));
        connectionTypeCombo->setStyleSheet(QString::fromUtf8("QComboBox {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 12px;\n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QComboBox:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QComboBox::drop-down {\n"
"    border: none;\n"
"}\n"
"\n"
"QComboBox::down-arrow {\n"
"    image: url(:static/styles/go-down.png);\n"
"    width: 16px;\n"
"    height: 16px;\n"
"}\n"
""));

        horizontalLayout->addWidget(connectionTypeCombo);

        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);


        verticalLayout->addWidget(connectionTypeGroup);

        networkStack = new QStackedWidget(NetworkPage);
        networkStack->setObjectName(QString::fromUtf8("networkStack"));
        networkStack->setSizeIncrement(QSize(540, 235));
        page = new QWidget();
        page->setObjectName(QString::fromUtf8("page"));
        ipSettingsGroup = new QGroupBox(page);
        ipSettingsGroup->setObjectName(QString::fromUtf8("ipSettingsGroup"));
        ipSettingsGroup->setGeometry(QRect(0, 10, 540, 219));
        ipSettingsGroup->setStyleSheet(QString::fromUtf8("QGroupBox {\n"
"    font-size: 16px;\n"
"    font-weight: bold;\n"
"    color: #333333;\n"
"    border: none;\n"
"    margin-bottom: 10px;\n"
"}\n"
""));
        gridLayout = new QGridLayout(ipSettingsGroup);
        gridLayout->setSpacing(15);
        gridLayout->setObjectName(QString::fromUtf8("gridLayout"));
        label_3 = new QLabel(ipSettingsGroup);
        label_3->setObjectName(QString::fromUtf8("label_3"));
        label_3->setStyleSheet(QString::fromUtf8("font-size: 14px;\n"
"color: #666666;"));

        gridLayout->addWidget(label_3, 2, 0, 1, 1);

        gatewayEdit = new QLineEdit(ipSettingsGroup);
        gatewayEdit->setObjectName(QString::fromUtf8("gatewayEdit"));
        gatewayEdit->setMinimumSize(QSize(0, 36));
        gatewayEdit->setStyleSheet(QString::fromUtf8("QLineEdit {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 12px;\n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QLineEdit:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:focus {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:disabled {\n"
"    background-color: #f5f7fa;\n"
"    color: #c0c4cc;\n"
"}\n"
""));

        gridLayout->addWidget(gatewayEdit, 3, 1, 1, 1);

        label_2 = new QLabel(ipSettingsGroup);
        label_2->setObjectName(QString::fromUtf8("label_2"));
        label_2->setStyleSheet(QString::fromUtf8("font-size: 14px;\n"
"color: #666666;"));

        gridLayout->addWidget(label_2, 1, 0, 1, 1);

        label_4 = new QLabel(ipSettingsGroup);
        label_4->setObjectName(QString::fromUtf8("label_4"));
        label_4->setStyleSheet(QString::fromUtf8("font-size: 14px;\n"
"color: #666666;"));

        gridLayout->addWidget(label_4, 3, 0, 1, 1);

        ipAddressEdit = new QLineEdit(ipSettingsGroup);
        ipAddressEdit->setObjectName(QString::fromUtf8("ipAddressEdit"));
        ipAddressEdit->setMinimumSize(QSize(0, 36));
        ipAddressEdit->setStyleSheet(QString::fromUtf8("QLineEdit {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 12px;\n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QLineEdit:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:focus {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:disabled {\n"
"    background-color: #f5f7fa;\n"
"    color: #c0c4cc;\n"
"}\n"
""));

        gridLayout->addWidget(ipAddressEdit, 1, 1, 1, 1);

        subnetMaskEdit = new QLineEdit(ipSettingsGroup);
        subnetMaskEdit->setObjectName(QString::fromUtf8("subnetMaskEdit"));
        subnetMaskEdit->setMinimumSize(QSize(0, 36));
        subnetMaskEdit->setStyleSheet(QString::fromUtf8("QLineEdit {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 12px;\n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QLineEdit:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:focus {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:disabled {\n"
"    background-color: #f5f7fa;\n"
"    color: #c0c4cc;\n"
"}\n"
""));

        gridLayout->addWidget(subnetMaskEdit, 2, 1, 1, 1);

        dnsEdit = new QLineEdit(ipSettingsGroup);
        dnsEdit->setObjectName(QString::fromUtf8("dnsEdit"));
        dnsEdit->setMinimumSize(QSize(0, 36));
        dnsEdit->setStyleSheet(QString::fromUtf8("QLineEdit {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 12px;\n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QLineEdit:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:focus {\n"
"    border-color: #409eff;\n"
"}\n"
"QLineEdit:disabled {\n"
"    background-color: #f5f7fa;\n"
"    color: #c0c4cc;\n"
"}\n"
""));

        gridLayout->addWidget(dnsEdit, 4, 1, 1, 1);

        label_5 = new QLabel(ipSettingsGroup);
        label_5->setObjectName(QString::fromUtf8("label_5"));
        label_5->setStyleSheet(QString::fromUtf8("font-size: 14px;\n"
"color: #666666;"));

        gridLayout->addWidget(label_5, 4, 0, 1, 1);

        ipModeComboBox = new QComboBox(ipSettingsGroup);
        ipModeComboBox->addItem(QString());
        ipModeComboBox->addItem(QString());
        ipModeComboBox->setObjectName(QString::fromUtf8("ipModeComboBox"));
        ipModeComboBox->setStyleSheet(QString::fromUtf8("QComboBox {\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    padding: 0 36px 0 12px; \n"
"    font-size: 14px;\n"
"    color: #333333;\n"
"    background-color: white;\n"
"}\n"
"QComboBox:hover {\n"
"    border-color: #409eff;\n"
"}\n"
"QComboBox::drop-down {\n"
"    subcontrol-origin: padding;\n"
"    subcontrol-position: top right;\n"
"    width: 32px;             \n"
"    border: none;\n"
"}\n"
"\n"
"QComboBox::down-arrow {\n"
"    image: url(:static/styles/go-down.png);\n"
"    width: 16px;\n"
"    height: 16px;\n"
"}\n"
""));

        gridLayout->addWidget(ipModeComboBox, 0, 1, 1, 1);

        networkStack->addWidget(page);
        page_2 = new QWidget();
        page_2->setObjectName(QString::fromUtf8("page_2"));
        networkStack->addWidget(page_2);

        verticalLayout->addWidget(networkStack);

        mainCard = new QWidget(NetworkPage);
        mainCard->setObjectName(QString::fromUtf8("mainCard"));
        mainCard->setStyleSheet(QString::fromUtf8("QWidget#mainCard {\n"
"    background-color: white;\n"
"    border-radius: 12px;\n"
"    padding: 20px;\n"
"}\n"
""));
        verticalLayout_2 = new QVBoxLayout(mainCard);
        verticalLayout_2->setSpacing(20);
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        verticalLayout_2->setContentsMargins(0, 0, 0, 0);

        verticalLayout->addWidget(mainCard);

        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setSpacing(16);
        horizontalLayout_2->setObjectName(QString::fromUtf8("horizontalLayout_2"));
        horizontalLayout_2->setContentsMargins(0, 0, 0, 0);
        horizontalSpacer_2 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_2->addItem(horizontalSpacer_2);

        cancelButton = new QPushButton(NetworkPage);
        cancelButton->setObjectName(QString::fromUtf8("cancelButton"));
        cancelButton->setMinimumSize(QSize(100, 40));
        cancelButton->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: 1px solid #e0e0e0;\n"
"    border-radius: 8px;\n"
"    color: #606266;\n"
"    font-size: 14px;\n"
"}\n"
"QPushButton:hover {\n"
"    border-color: #409eff;\n"
"    color: #409eff;\n"
"}\n"
"QPushButton:pressed {\n"
"    border-color: #337ecc;\n"
"    color: #337ecc;\n"
"}\n"
""));

        horizontalLayout_2->addWidget(cancelButton);

        applyButton = new QPushButton(NetworkPage);
        applyButton->setObjectName(QString::fromUtf8("applyButton"));
        applyButton->setMinimumSize(QSize(100, 40));
        applyButton->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: #409eff;\n"
"    border: none;\n"
"    border-radius: 8px;\n"
"    color: white;\n"
"    font-size: 14px;\n"
"}\n"
"QPushButton:hover {\n"
"    background-color: #66b1ff;\n"
"}\n"
"QPushButton:pressed {\n"
"    background-color: #337ecc;\n"
"}\n"
""));

        horizontalLayout_2->addWidget(applyButton);


        verticalLayout->addLayout(horizontalLayout_2);


        retranslateUi(NetworkPage);

        networkStack->setCurrentIndex(0);


        QMetaObject::connectSlotsByName(NetworkPage);
    } // setupUi

    void retranslateUi(QWidget *NetworkPage)
    {
        NetworkPage->setWindowTitle(QApplication::translate("NetworkPage", "NetworkPage", nullptr));
        titleLabel->setText(QApplication::translate("NetworkPage", "\347\275\221\347\273\234\350\277\236\346\216\245\350\256\276\347\275\256", nullptr));
        connectionTypeGroup->setTitle(QString());
        label->setText(QApplication::translate("NetworkPage", "\351\200\211\346\213\251\347\275\221\347\273\234\347\261\273\345\236\213:", nullptr));
        connectionTypeCombo->setItemText(0, QApplication::translate("NetworkPage", "\346\234\211\347\272\277\347\275\221\347\273\234", nullptr));
        connectionTypeCombo->setItemText(1, QApplication::translate("NetworkPage", "\346\227\240\347\272\277\347\275\221\347\273\234", nullptr));

        ipSettingsGroup->setTitle(QApplication::translate("NetworkPage", "IP\350\256\276\347\275\256", nullptr));
        label_3->setText(QApplication::translate("NetworkPage", "\345\255\220\347\275\221\346\216\251\347\240\201:", nullptr));
        gatewayEdit->setText(QString());
        label_2->setText(QApplication::translate("NetworkPage", "IP\345\234\260\345\235\200:", nullptr));
        label_4->setText(QApplication::translate("NetworkPage", "\347\275\221\345\205\263:", nullptr));
        ipAddressEdit->setText(QString());
        subnetMaskEdit->setText(QString());
        dnsEdit->setText(QString());
        label_5->setText(QApplication::translate("NetworkPage", "DNS\346\234\215\345\212\241\345\231\250:", nullptr));
        ipModeComboBox->setItemText(0, QApplication::translate("NetworkPage", "\345\212\250\346\200\201(DHCP)", nullptr));
        ipModeComboBox->setItemText(1, QApplication::translate("NetworkPage", "\351\235\231\346\200\201IP", nullptr));

        cancelButton->setText(QApplication::translate("NetworkPage", "\345\217\226\346\266\210", nullptr));
        applyButton->setText(QApplication::translate("NetworkPage", "\345\272\224\347\224\250", nullptr));
    } // retranslateUi

};

namespace Ui {
    class NetworkPage: public Ui_NetworkPage {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_NETWORKPAGE_H
