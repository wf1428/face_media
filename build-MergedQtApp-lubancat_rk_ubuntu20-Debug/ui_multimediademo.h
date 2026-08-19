/********************************************************************************
** Form generated from reading UI file 'multimediademo.ui'
**
** Created by: Qt User Interface Compiler version 5.12.8
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MULTIMEDIADEMO_H
#define UI_MULTIMEDIADEMO_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MultimediaDemo
{
public:
    QWidget *verticalLayoutWidget;

    void setupUi(QWidget *MultimediaDemo)
    {
        if (MultimediaDemo->objectName().isEmpty())
            MultimediaDemo->setObjectName(QString::fromUtf8("MultimediaDemo"));
        MultimediaDemo->resize(400, 200);
        verticalLayoutWidget = new QWidget(MultimediaDemo);
        verticalLayoutWidget->setObjectName(QString::fromUtf8("verticalLayoutWidget"));
        verticalLayoutWidget->setGeometry(QRect(10, 10, 381, 181));

        retranslateUi(MultimediaDemo);

        QMetaObject::connectSlotsByName(MultimediaDemo);
    } // setupUi

    void retranslateUi(QWidget *MultimediaDemo)
    {
        MultimediaDemo->setWindowTitle(QApplication::translate("MultimediaDemo", "\345\244\232\345\252\222\344\275\223\346\274\224\347\244\272", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MultimediaDemo: public Ui_MultimediaDemo {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MULTIMEDIADEMO_H
