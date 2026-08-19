/********************************************************************************
** Form generated from reading UI file 'mainwindow.ui'
**
** Created by: Qt User Interface Compiler version 5.12.8
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAINWINDOW_H
#define UI_MAINWINDOW_H

#include <QtCore/QVariant>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QWidget *centralwidget;
    QVBoxLayout *verticalLayout;
    QLabel *titleLabel;
    QGridLayout *gridLayout;
    QWidget *appWidget1;
    QVBoxLayout *verticalLayout_1;
    QPushButton *appButton1;
    QLabel *appLabel1;
    QWidget *appWidget2;
    QVBoxLayout *verticalLayout_2;
    QPushButton *appButton2;
    QLabel *appLabel2;
    QWidget *appWidget3;
    QVBoxLayout *verticalLayout_3;
    QPushButton *appButton3;
    QLabel *appLabel3;
    QWidget *appWidget4;
    QVBoxLayout *verticalLayout_4;
    QPushButton *appButton4;
    QLabel *appLabel4;
    QWidget *appWidget5;
    QVBoxLayout *verticalLayout_5;
    QPushButton *appButton5;
    QLabel *appLabel5;
    QWidget *exitWidget;
    QVBoxLayout *verticalLayout_exit;
    QPushButton *exitButton;
    QLabel *exitLabel;
    QSpacerItem *verticalSpacer;
    QMenuBar *menubar;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName(QString::fromUtf8("MainWindow"));
        MainWindow->resize(1024, 768);
        MainWindow->setStyleSheet(QString::fromUtf8("QMainWindow { background-color: #f5f7fa; }"));
        centralwidget = new QWidget(MainWindow);
        centralwidget->setObjectName(QString::fromUtf8("centralwidget"));
        centralwidget->setStyleSheet(QString::fromUtf8("background-color: #f5f7fa;"));
        verticalLayout = new QVBoxLayout(centralwidget);
        verticalLayout->setSpacing(30);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        verticalLayout->setContentsMargins(40, 40, 40, 40);
        titleLabel = new QLabel(centralwidget);
        titleLabel->setObjectName(QString::fromUtf8("titleLabel"));
        titleLabel->setStyleSheet(QString::fromUtf8("font-size: 28px;\n"
"font-weight: 600;\n"
"color: #303133;\n"
"margin-bottom: 20px;"));

        verticalLayout->addWidget(titleLabel);

        gridLayout = new QGridLayout();
        gridLayout->setSpacing(40);
        gridLayout->setObjectName(QString::fromUtf8("gridLayout"));
        gridLayout->setContentsMargins(20, 20, 20, 20);
        appWidget1 = new QWidget(centralwidget);
        appWidget1->setObjectName(QString::fromUtf8("appWidget1"));
        appWidget1->setMaximumSize(QSize(140, 160));
        verticalLayout_1 = new QVBoxLayout(appWidget1);
        verticalLayout_1->setSpacing(12);
        verticalLayout_1->setObjectName(QString::fromUtf8("verticalLayout_1"));
        verticalLayout_1->setContentsMargins(20, 0, 20, 0);
        appButton1 = new QPushButton(appWidget1);
        appButton1->setObjectName(QString::fromUtf8("appButton1"));
        QSizePolicy sizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(appButton1->sizePolicy().hasHeightForWidth());
        appButton1->setSizePolicy(sizePolicy);
        appButton1->setMinimumSize(QSize(100, 100));
        appButton1->setMaximumSize(QSize(100, 100));
        appButton1->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"}\n"
"QPushButton:pressed {\n"
"    background-color: #f0f0f0;\n"
"}"));
        QIcon icon;
        icon.addFile(QString::fromUtf8(":/static/icons/system.png"), QSize(), QIcon::Normal, QIcon::Off);
        appButton1->setIcon(icon);
        appButton1->setIconSize(QSize(64, 64));

        verticalLayout_1->addWidget(appButton1);

        appLabel1 = new QLabel(appWidget1);
        appLabel1->setObjectName(QString::fromUtf8("appLabel1"));
        appLabel1->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #606266;"));
        appLabel1->setAlignment(Qt::AlignCenter);

        verticalLayout_1->addWidget(appLabel1);


        gridLayout->addWidget(appWidget1, 0, 0, 1, 1);

        appWidget2 = new QWidget(centralwidget);
        appWidget2->setObjectName(QString::fromUtf8("appWidget2"));
        appWidget2->setMaximumSize(QSize(140, 160));
        verticalLayout_2 = new QVBoxLayout(appWidget2);
        verticalLayout_2->setSpacing(12);
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        verticalLayout_2->setContentsMargins(20, 0, 20, 0);
        appButton2 = new QPushButton(appWidget2);
        appButton2->setObjectName(QString::fromUtf8("appButton2"));
        sizePolicy.setHeightForWidth(appButton2->sizePolicy().hasHeightForWidth());
        appButton2->setSizePolicy(sizePolicy);
        appButton2->setMinimumSize(QSize(100, 100));
        appButton2->setMaximumSize(QSize(100, 100));
        appButton2->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"}\n"
"QPushButton:pressed {\n"
"    background-color: #f0f0f0;\n"
"}"));
        QIcon icon1;
        icon1.addFile(QString::fromUtf8(":/static/icons/applications.png"), QSize(), QIcon::Normal, QIcon::Off);
        appButton2->setIcon(icon1);
        appButton2->setIconSize(QSize(64, 64));

        verticalLayout_2->addWidget(appButton2);

        appLabel2 = new QLabel(appWidget2);
        appLabel2->setObjectName(QString::fromUtf8("appLabel2"));
        appLabel2->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #606266;"));
        appLabel2->setAlignment(Qt::AlignCenter);

        verticalLayout_2->addWidget(appLabel2);


        gridLayout->addWidget(appWidget2, 0, 1, 1, 1);

        appWidget3 = new QWidget(centralwidget);
        appWidget3->setObjectName(QString::fromUtf8("appWidget3"));
        appWidget3->setMaximumSize(QSize(140, 160));
        verticalLayout_3 = new QVBoxLayout(appWidget3);
        verticalLayout_3->setSpacing(12);
        verticalLayout_3->setObjectName(QString::fromUtf8("verticalLayout_3"));
        verticalLayout_3->setContentsMargins(20, 0, 20, 0);
        appButton3 = new QPushButton(appWidget3);
        appButton3->setObjectName(QString::fromUtf8("appButton3"));
        sizePolicy.setHeightForWidth(appButton3->sizePolicy().hasHeightForWidth());
        appButton3->setSizePolicy(sizePolicy);
        appButton3->setMinimumSize(QSize(100, 100));
        appButton3->setMaximumSize(QSize(100, 100));
        appButton3->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"}\n"
"QPushButton:pressed {\n"
"\n"
"}"));
        QIcon icon2;
        icon2.addFile(QString::fromUtf8(":/static/icons/ui_media.png"), QSize(), QIcon::Normal, QIcon::Off);
        appButton3->setIcon(icon2);
        appButton3->setIconSize(QSize(64, 64));

        verticalLayout_3->addWidget(appButton3);

        appLabel3 = new QLabel(appWidget3);
        appLabel3->setObjectName(QString::fromUtf8("appLabel3"));
        appLabel3->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #606266;"));
        appLabel3->setAlignment(Qt::AlignCenter);

        verticalLayout_3->addWidget(appLabel3);


        gridLayout->addWidget(appWidget3, 0, 2, 1, 1);

        appWidget4 = new QWidget(centralwidget);
        appWidget4->setObjectName(QString::fromUtf8("appWidget4"));
        appWidget4->setMaximumSize(QSize(140, 160));
        verticalLayout_4 = new QVBoxLayout(appWidget4);
        verticalLayout_4->setSpacing(12);
        verticalLayout_4->setObjectName(QString::fromUtf8("verticalLayout_4"));
        verticalLayout_4->setContentsMargins(20, 0, 20, 0);
        appButton4 = new QPushButton(appWidget4);
        appButton4->setObjectName(QString::fromUtf8("appButton4"));
        sizePolicy.setHeightForWidth(appButton4->sizePolicy().hasHeightForWidth());
        appButton4->setSizePolicy(sizePolicy);
        appButton4->setMinimumSize(QSize(100, 100));
        appButton4->setMaximumSize(QSize(100, 100));
        QFont font;
        font.setFamily(QString::fromUtf8("Microsoft YaHei"));
        font.setPointSize(28);
        font.setBold(true);
        font.setWeight(75);
        appButton4->setFont(font);
        appButton4->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"}\n"
"QPushButton:pressed {\n"
"\n"
"}"));
        QIcon icon3;
        icon3.addFile(QString::fromUtf8(":/static/icons/debug.png"), QSize(), QIcon::Normal, QIcon::Off);
        appButton4->setIcon(icon3);
        appButton4->setIconSize(QSize(64, 64));

        verticalLayout_4->addWidget(appButton4);

        appLabel4 = new QLabel(appWidget4);
        appLabel4->setObjectName(QString::fromUtf8("appLabel4"));
        appLabel4->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #606266;"));
        appLabel4->setAlignment(Qt::AlignCenter);

        verticalLayout_4->addWidget(appLabel4);


        gridLayout->addWidget(appWidget4, 1, 0, 1, 1);

        appWidget5 = new QWidget(centralwidget);
        appWidget5->setObjectName(QString::fromUtf8("appWidget5"));
        appWidget5->setMaximumSize(QSize(140, 160));
        verticalLayout_5 = new QVBoxLayout(appWidget5);
        verticalLayout_5->setSpacing(12);
        verticalLayout_5->setObjectName(QString::fromUtf8("verticalLayout_5"));
        verticalLayout_5->setContentsMargins(20, 0, 20, 0);
        appButton5 = new QPushButton(appWidget5);
        appButton5->setObjectName(QString::fromUtf8("appButton5"));
        sizePolicy.setHeightForWidth(appButton5->sizePolicy().hasHeightForWidth());
        appButton5->setSizePolicy(sizePolicy);
        appButton5->setMinimumSize(QSize(100, 100));
        appButton5->setMaximumSize(QSize(100, 100));
        appButton5->setFont(font);
        appButton5->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"}\n"
"QPushButton:pressed {\n"
"\n"
"}"));
        QIcon icon4;
        icon4.addFile(QString::fromUtf8(":/static/icons/file_manage.png"), QSize(), QIcon::Normal, QIcon::Off);
        appButton5->setIcon(icon4);
        appButton5->setIconSize(QSize(64, 64));

        verticalLayout_5->addWidget(appButton5);

        appLabel5 = new QLabel(appWidget5);
        appLabel5->setObjectName(QString::fromUtf8("appLabel5"));
        appLabel5->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #606266;"));
        appLabel5->setAlignment(Qt::AlignCenter);

        verticalLayout_5->addWidget(appLabel5);


        gridLayout->addWidget(appWidget5, 1, 1, 1, 1);

        exitWidget = new QWidget(centralwidget);
        exitWidget->setObjectName(QString::fromUtf8("exitWidget"));
        exitWidget->setMaximumSize(QSize(140, 160));
        verticalLayout_exit = new QVBoxLayout(exitWidget);
        verticalLayout_exit->setSpacing(12);
        verticalLayout_exit->setObjectName(QString::fromUtf8("verticalLayout_exit"));
        verticalLayout_exit->setContentsMargins(20, 0, 20, 0);
        exitButton = new QPushButton(exitWidget);
        exitButton->setObjectName(QString::fromUtf8("exitButton"));
        sizePolicy.setHeightForWidth(exitButton->sizePolicy().hasHeightForWidth());
        exitButton->setSizePolicy(sizePolicy);
        exitButton->setMinimumSize(QSize(100, 100));
        exitButton->setMaximumSize(QSize(100, 100));
        exitButton->setFont(font);
        exitButton->setStyleSheet(QString::fromUtf8("QPushButton {\n"
"    background-color: white;\n"
"    border: none;\n"
"    border-radius: 16px;\n"
"\n"
"}\n"
"QPushButton:hover {\n"
"\n"
"    background-color: #fff2f0;\n"
"}\n"
"QPushButton:pressed {\n"
"\n"
"    background-color: #ffe6e6;\n"
"}"));
        QIcon icon5;
        icon5.addFile(QString::fromUtf8(":/static/icons/shutdown.png"), QSize(), QIcon::Normal, QIcon::Off);
        exitButton->setIcon(icon5);
        exitButton->setIconSize(QSize(64, 64));

        verticalLayout_exit->addWidget(exitButton);

        exitLabel = new QLabel(exitWidget);
        exitLabel->setObjectName(QString::fromUtf8("exitLabel"));
        exitLabel->setStyleSheet(QString::fromUtf8("font-size: 16px;\n"
"font-weight: 500;\n"
"color: #ff5252;"));
        exitLabel->setAlignment(Qt::AlignCenter);

        verticalLayout_exit->addWidget(exitLabel);


        gridLayout->addWidget(exitWidget, 1, 2, 1, 1);


        verticalLayout->addLayout(gridLayout);

        verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

        verticalLayout->addItem(verticalSpacer);

        MainWindow->setCentralWidget(centralwidget);
        menubar = new QMenuBar(MainWindow);
        menubar->setObjectName(QString::fromUtf8("menubar"));
        menubar->setGeometry(QRect(0, 0, 1024, 28));
        menubar->setStyleSheet(QString::fromUtf8("background-color: transparent;\n"
"border: none;"));
        MainWindow->setMenuBar(menubar);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName(QString::fromUtf8("statusbar"));
        statusbar->setStyleSheet(QString::fromUtf8("background-color: transparent;\n"
"border: none;\n"
"color: #909399;"));
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QApplication::translate("MainWindow", "\346\231\272\350\203\275\350\256\276\345\244\207\346\216\247\345\210\266\345\217\260", nullptr));
        titleLabel->setText(QApplication::translate("MainWindow", "\346\231\272\350\203\275\350\256\276\345\244\207\346\216\247\345\210\266\345\217\260", nullptr));
        appButton1->setText(QString());
        appLabel1->setText(QApplication::translate("MainWindow", "\350\256\276\347\275\256", nullptr));
        appButton2->setText(QString());
        appLabel2->setText(QApplication::translate("MainWindow", "\345\212\237\350\203\275\347\256\241\347\220\206", nullptr));
        appButton3->setText(QString());
        appLabel3->setText(QApplication::translate("MainWindow", "\344\270\273\347\225\214\351\235\242", nullptr));
        appButton4->setText(QString());
        appLabel4->setText(QApplication::translate("MainWindow", "\350\260\203\350\257\225", nullptr));
        appButton5->setText(QString());
        appLabel5->setText(QApplication::translate("MainWindow", "<html><head/><body><p>\346\226\207\344\273\266\347\256\241\347\220\206</p></body></html>", nullptr));
        exitButton->setText(QString());
        exitLabel->setText(QApplication::translate("MainWindow", "\351\207\215\345\220\257", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAINWINDOW_H
