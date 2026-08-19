/********************************************************************************
** Form generated from reading UI file 'featuresdialog.ui'
**
** Created by: Qt User Interface Compiler version 5.12.8
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_FEATURESDIALOG_H
#define UI_FEATURESDIALOG_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QVBoxLayout>

QT_BEGIN_NAMESPACE

class Ui_FeaturesDialog
{
public:
    QVBoxLayout *verticalLayout;
    QVBoxLayout *verticalLayout_3;
    QLabel *titleLabel;
    QGroupBox *featuresGroup;
    QVBoxLayout *verticalLayout_2;
    QCheckBox *feature1;
    QCheckBox *feature2;
    QCheckBox *feature3;
    QCheckBox *feature4;
    QCheckBox *feature5;
    QCheckBox *feature6;
    QCheckBox *feature7;
    QFrame *divider;
    QLabel *statusLabel;
    QHBoxLayout *horizontalLayout;
    QSpacerItem *horizontalSpacer;
    QPushButton *closeButton;
    QPushButton *applyButton;

    void setupUi(QDialog *FeaturesDialog)
    {
        if (FeaturesDialog->objectName().isEmpty())
            FeaturesDialog->setObjectName(QString::fromUtf8("FeaturesDialog"));
        FeaturesDialog->resize(450, 625);
        FeaturesDialog->setStyleSheet(QString::fromUtf8("/* Element-UI \351\243\216\346\240\274\346\240\267\345\274\217 */\n"
"QDialog { \n"
"    background-color: #f5f7fa; \n"
"    border-radius: 8px;\n"
"}\n"
"\n"
"/* \345\215\241\347\211\207\346\240\267\345\274\217 */\n"
"QWidget#mainCard { \n"
"    background-color: white; \n"
"    border-radius: 12px; \n"
"    padding: 24px;\n"
"}\n"
"\n"
"/* \346\240\207\351\242\230\346\240\267\345\274\217 */\n"
"QLabel#titleLabel {\n"
"    color: #303133;\n"
"    font-size: 20px;\n"
"    font-weight: 600;\n"
"    margin-bottom: 20px;\n"
"}\n"
"\n"
"/* \345\210\206\347\273\204\346\241\206\346\240\267\345\274\217 */\n"
"QGroupBox { \n"
"    border: none; \n"
"    margin-top: 0;\n"
"    background-color: transparent;\n"
"}\n"
"\n"
"QGroupBox::title { \n"
"    subcontrol-origin: margin; \n"
"    left: 0; \n"
"    top: 0; \n"
"    padding: 0;\n"
"    background-color: transparent; \n"
"    color: #606266; \n"
"    font-size: 16px; \n"
"    font-weight: 500; \n"
"}\n"
"\n"
"/* \345\244\215\351\200\211\346\241\206\346\240\267\345\274"
                        "\217 */\n"
"QCheckBox { \n"
"    spacing: 12px; \n"
"    margin: 16px 0; \n"
"    color: #606266; \n"
"    font-size: 14px; \n"
"}\n"
"\n"
"QCheckBox::indicator { \n"
"    width: 20px; \n"
"    height: 20px; \n"
"    border: 2px solid #dcdfe6; \n"
"    border-radius: 4px; \n"
"    background-color: white; \n"
"\n"
"}\n"
"\n"
"QCheckBox::indicator:hover { \n"
"    border-color: #409eff; \n"
"}\n"
"\n"
"QCheckBox::indicator:checked { \n"
"    background-color: #409eff; \n"
"    border-color: #409eff; \n"
"    image: url(:/icons/check.png); \n"
"}\n"
"\n"
"/* \347\212\266\346\200\201\346\240\207\347\255\276\346\240\267\345\274\217 */\n"
"QLabel#statusLabel { \n"
"    color: #909399; \n"
"    font-size: 14px; \n"
"    margin: 16px 0; \n"
"    padding: 12px; \n"
"    background-color: #ecf5ff; \n"
"    border: 1px solid #ebeef5; \n"
"    border-radius: 6px;\n"
"}\n"
"\n"
"/* \346\214\211\351\222\256\346\240\267\345\274\217 */\n"
"QPushButton { \n"
"    padding: 12px 24px; \n"
"    border: 1px solid #dcdfe6; \n"
"  "
                        "  border-radius: 6px; \n"
"    background-color: white; \n"
"    color: #606266; \n"
"    font-size: 14px; \n"
"    font-weight: 500; \n"
"    min-width: 120px;\n"
"}\n"
"\n"
"QPushButton:hover { \n"
"    border-color: #409eff; \n"
"    background-color: #ecf5ff; \n"
"    color: #409eff; \n"
"}\n"
"\n"
"QPushButton:pressed { \n"
"    border-color: #337ecc; \n"
"    background-color: #d9ecff; \n"
"    color: #337ecc; \n"
"}\n"
"\n"
"QPushButton:default { \n"
"    border-color: #409eff; \n"
"    background-color: #409eff; \n"
"    color: white; \n"
"}\n"
"\n"
"QPushButton:default:hover { \n"
"    background-color: #66b1ff; \n"
"    border-color: #66b1ff; \n"
"}\n"
"\n"
"QPushButton:default:pressed { \n"
"    background-color: #337ecc; \n"
"    border-color: #337ecc; \n"
"}\n"
"\n"
"/* \345\210\206\345\211\262\347\272\277\346\240\267\345\274\217 */\n"
"QFrame#divider { \n"
"    height: 1px; \n"
"    background-color: #ebeef5; \n"
"    margin: 20px 0;\n"
"}"));
        FeaturesDialog->setModal(false);
        verticalLayout = new QVBoxLayout(FeaturesDialog);
        verticalLayout->setSpacing(0);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        verticalLayout->setContentsMargins(16, 16, 16, 16);
        verticalLayout_3 = new QVBoxLayout();
        verticalLayout_3->setSpacing(0);
        verticalLayout_3->setObjectName(QString::fromUtf8("verticalLayout_3"));
        verticalLayout_3->setContentsMargins(0, 0, 0, 0);
        titleLabel = new QLabel(FeaturesDialog);
        titleLabel->setObjectName(QString::fromUtf8("titleLabel"));

        verticalLayout_3->addWidget(titleLabel);

        featuresGroup = new QGroupBox(FeaturesDialog);
        featuresGroup->setObjectName(QString::fromUtf8("featuresGroup"));
        verticalLayout_2 = new QVBoxLayout(featuresGroup);
        verticalLayout_2->setSpacing(0);
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        verticalLayout_2->setContentsMargins(0, 16, 0, 0);
        feature1 = new QCheckBox(featuresGroup);
        feature1->setObjectName(QString::fromUtf8("feature1"));

        verticalLayout_2->addWidget(feature1);

        feature2 = new QCheckBox(featuresGroup);
        feature2->setObjectName(QString::fromUtf8("feature2"));

        verticalLayout_2->addWidget(feature2);

        feature3 = new QCheckBox(featuresGroup);
        feature3->setObjectName(QString::fromUtf8("feature3"));

        verticalLayout_2->addWidget(feature3);

        feature4 = new QCheckBox(featuresGroup);
        feature4->setObjectName(QString::fromUtf8("feature4"));

        verticalLayout_2->addWidget(feature4);

        feature5 = new QCheckBox(featuresGroup);
        feature5->setObjectName(QString::fromUtf8("feature5"));

        verticalLayout_2->addWidget(feature5);

        feature6 = new QCheckBox(featuresGroup);
        feature6->setObjectName(QString::fromUtf8("feature6"));

        verticalLayout_2->addWidget(feature6);

        feature7 = new QCheckBox(featuresGroup);
        feature7->setObjectName(QString::fromUtf8("feature7"));

        verticalLayout_2->addWidget(feature7);


        verticalLayout_3->addWidget(featuresGroup);

        divider = new QFrame(FeaturesDialog);
        divider->setObjectName(QString::fromUtf8("divider"));
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Sunken);

        verticalLayout_3->addWidget(divider);

        statusLabel = new QLabel(FeaturesDialog);
        statusLabel->setObjectName(QString::fromUtf8("statusLabel"));
        statusLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);

        verticalLayout_3->addWidget(statusLabel);

        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setSpacing(16);
        horizontalLayout->setObjectName(QString::fromUtf8("horizontalLayout"));
        horizontalLayout->setContentsMargins(0, 0, 0, 0);
        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);

        closeButton = new QPushButton(FeaturesDialog);
        closeButton->setObjectName(QString::fromUtf8("closeButton"));
        QSizePolicy sizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(closeButton->sizePolicy().hasHeightForWidth());
        closeButton->setSizePolicy(sizePolicy);

        horizontalLayout->addWidget(closeButton);

        applyButton = new QPushButton(FeaturesDialog);
        applyButton->setObjectName(QString::fromUtf8("applyButton"));
        sizePolicy.setHeightForWidth(applyButton->sizePolicy().hasHeightForWidth());
        applyButton->setSizePolicy(sizePolicy);

        horizontalLayout->addWidget(applyButton);


        verticalLayout_3->addLayout(horizontalLayout);


        verticalLayout->addLayout(verticalLayout_3);


        retranslateUi(FeaturesDialog);
        QObject::connect(closeButton, SIGNAL(clicked()), FeaturesDialog, SLOT(reject()));
        QObject::connect(applyButton, SIGNAL(clicked()), FeaturesDialog, SLOT(accept()));

        applyButton->setDefault(true);


        QMetaObject::connectSlotsByName(FeaturesDialog);
    } // setupUi

    void retranslateUi(QDialog *FeaturesDialog)
    {
        FeaturesDialog->setWindowTitle(QApplication::translate("FeaturesDialog", "\345\212\237\350\203\275\351\200\211\351\241\271", nullptr));
        titleLabel->setText(QApplication::translate("FeaturesDialog", "\345\212\237\350\203\275\351\200\211\351\241\271", nullptr));
        featuresGroup->setTitle(QApplication::translate("FeaturesDialog", "\350\257\267\351\200\211\346\213\251\350\246\201\345\220\257\347\224\250\347\232\204\345\212\237\350\203\275", nullptr));
        feature1->setText(QApplication::translate("FeaturesDialog", "\347\246\273\347\272\277\347\211\210\346\242\257\346\216\247\347\263\273\347\273\237 V1", nullptr));
        feature2->setText(QApplication::translate("FeaturesDialog", "\347\275\221\347\273\234\347\211\210\346\242\257\346\216\247\347\263\273\347\273\237 V1", nullptr));
        feature3->setText(QApplication::translate("FeaturesDialog", "\347\275\221\347\273\234\347\211\210\346\242\257\346\216\247\347\263\273\347\273\237 V2", nullptr));
        feature4->setText(QApplication::translate("FeaturesDialog", "\347\224\265\346\242\257\347\251\272\350\260\203\347\263\273\347\273\237", nullptr));
        feature5->setText(QApplication::translate("FeaturesDialog", "\345\244\232\345\252\222\344\275\223\347\224\265\346\242\257\347\263\273\347\273\237", nullptr));
        feature6->setText(QApplication::translate("FeaturesDialog", "\347\224\265\346\242\257\345\271\277\346\222\255\347\263\273\347\273\237", nullptr));
        feature7->setText(QApplication::translate("FeaturesDialog", "\345\217\257\350\247\206\345\257\271\350\256\262\347\263\273\347\273\237", nullptr));
        statusLabel->setText(QApplication::translate("FeaturesDialog", "\350\257\267\351\200\211\346\213\251\345\212\237\350\203\275\351\200\211\351\241\271", nullptr));
        closeButton->setText(QApplication::translate("FeaturesDialog", "\345\205\263\351\227\255", nullptr));
        applyButton->setText(QApplication::translate("FeaturesDialog", "\345\272\224\347\224\250", nullptr));
    } // retranslateUi

};

namespace Ui {
    class FeaturesDialog: public Ui_FeaturesDialog {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_FEATURESDIALOG_H
