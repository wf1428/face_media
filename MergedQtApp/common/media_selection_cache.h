/**
 * @file media_selection_cache.h
 * @brief 后台刷新媒体选择配置，界面只读取无锁内存值。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "configuration_work.h"
#include "media_content_policy.h"
#include <QTimer>
#include <QPointer>
#include <QSettings>
#include <memory>

/** @brief UI线程所有的配置快照；每秒后台读INI，最多一个刷新在途。
 * @details 所有公开方法及回调仅在UI线程执行，磁盘访问只在media.config后台。
 * 刷新与已有配置写入共用串行队列；对象销毁后结果丢弃，失败保留上一快照。
 * 构造种子来自启动已读取的INI；后台任务关闭原慢段输出，不新增日志。
 * 配置变更在下次成功刷新后可见（空闲时通常1秒内，配置队列忙时顺延）；不会为刷新修改文件。
 */
class MediaSelectionCache final:public QObject {
public:
    /** @brief 保存启动值并启动周期刷新，不在构造线程读取文件。
     * @param[in] path 固定配置路径，后台按原INI键与默认值读取。
     * @param[in] type 已规范化启动contentType。
     * @param[in] small 启动smallImageMode。
     * @param[in] parent UI所有者，销毁后取消待执行/完成通知。
     */
    MediaSelectionCache(QString path,QString type,bool small,QObject *parent=nullptr)
        :QObject(parent),path_(std::move(path)),type_(std::move(type)),small_(small) {
        timer_.setParent(this);timer_.setInterval(1000);
        connect(&timer_,&QTimer::timeout,this,[this]{refresh();});timer_.start();
    }
    /** @return 最近成功刷新contentType；仅UI读取，不访问文件。 */
    QString contentType() const { return type_; }
    /** @return 最近成功刷新smallImageMode；仅UI读取，不访问文件。 */
    bool smallImageMode() const { return small_; }
    /** @brief 合并后台刷新；已在途时跳过，失败留旧值并等待下一周期。 */
    void refresh() {
        if(pending_) return;
        pending_=true;
        /** @brief 后台写入、完成后UI读取的一次快照；串行任务交接建立先后关系。 */
        struct Values {
            QString type;bool small=false; /**< 标准化contentType及原小图开关语义。 */
            bool valid=false; /**< true读取无QSettings错误，false保留UI旧值。 */
        };
        const auto values=std::make_shared<Values>();const auto path=path_;const QPointer<MediaSelectionCache> alive(this);
        ConfigurationWork::queue().enqueue([alive,path,values] {
            SerialWorkQueue::Task task;task.reportSlow=false;
            task.run=[path,values] {
                QSettings settings(path,QSettings::IniFormat);settings.setIniCodec("UTF-8");
                values->type=MediaContentPolicy::normalizeContentType(settings.value("playback/contentType","VIDEO_IMAGE").toString());
                values->small=settings.value("playback/smallImageMode",0).toInt()==1;
                values->valid=settings.status()==QSettings::NoError;return QString();
            };
            task.done=[alive,values](const QString &error) {
                if(!alive) return;
                if(error.isEmpty() && values->valid) { alive->type_=values->type;alive->small_=values->small; }
                alive->pending_=false;
            };return task;
        },QStringLiteral("selection-read-%1").arg(quintptr(this)),[alive]{return !alive;});
    }
private:
    QString path_,type_;bool small_=false,pending_=false; /**< UI独占路径、上次成功值、刷新在途门；无共享锁。 */
    QTimer timer_; /**< UI只排队，1000ms周期；不执行文件查询。 */
};
