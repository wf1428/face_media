/**
 * @file camera_screenshot_source.h
 * @brief 在主线程交接摄像头截图需求和自有图像，不连接 PIC 或识别预览。
 * @author Du lin
 * @date 2026-11-05
 */
#pragma once
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QCoreApplication>
#include <QThread>
#include <functional>

/**
 * @brief 单设备截图源的主线程桥接；不拥有相机、窗口或图像工作线程。
 * @details 相机窗口注册受 QPointer 保护的回调；上传器只请求需求和领取独立截图帧。
 * 所有接口仅在应用主线程调用。后台编码只能使用领取后的自有 QImage 值。
 */
class CameraScreenshotSource {
public:
    /** @return 进程内唯一截图源；不会创建相机或启动采集。 */
    static CameraScreenshotSource &instance() {
        Q_ASSERT(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread());
        static CameraScreenshotSource value; return value;
    }
    /** @brief 注册相机所有者并应用已存在的后台需求。
     * @param[in] owner 借用的主线程对象；销毁后不调用任何回调。
     * @param[in] demand 更新截图需求，不激活识别模块或显示预览。
     * @param[in] take 非阻塞领取自有图像和采集 epoch 毫秒，尚未到帧返回 false。
     */
    void bind(QObject *owner,std::function<void(bool)> demand,std::function<bool(QImage*,qint64*)> take) {
        owner_=owner; demandCallback_=std::move(demand); take_=std::move(take);
        if(owner_ && demandCallback_) demandCallback_(wanted_);
    }
    /** @brief 仅匹配的相机所有者可解除注册；保留逻辑需求供后续重新绑定。
     * @param[in] owner 原 bind 对象；不匹配时忽略。
     */
    void unbind(QObject *owner) {
        if(owner_!=owner)return;
        owner_.clear(); demandCallback_={}; take_={};
    }
    /** @brief 改变后台采集需求；重复值不触发相机启停。
     * @param[in] wanted true 允许仅截图采集；false 释放截图占用，识别占用由相机所有者保留。
     */
    void setDemand(bool wanted) {
        if(wanted_==wanted)return;
        wanted_=wanted;
        if(owner_ && demandCallback_) demandCallback_(wanted);
    }
    /** @brief 领取独立截图帧，同时向采集线程请求新帧；不消费识别邮箱。
     * @param[out] image 已取得结果时为自有只读像素；空图表示转换失败，未就绪时保持原值。
     * @param[out] ts 成功时为采集时刻 epoch 毫秒。
     * @return true已得到采集结果（空图为转换失败）；false无源、未启用或尚无结果。
     */
    bool take(QImage *image,qint64 *ts) { return wanted_ && owner_ && take_ && take_(image,ts); }
private:
    QPointer<QObject> owner_; /**< 非拥有的主线程相机窗口。 */
    std::function<void(bool)> demandCallback_; /**< 主线程更新需求，不等待采集线程。 */
    std::function<bool(QImage*,qint64*)> take_; /**< 主线程有界领取自有帧。 */
    bool wanted_=false; /**< true 为上传器仍占用采集；缺省无后台需求。 */
};
