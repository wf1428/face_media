/**
 * @file IApplicationModule.h
 * @brief 可由应用外壳统一管理的业务模块接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IAPPLICATIONMODULE_H
#define IAPPLICATIONMODULE_H

class QWidget;

/**
 * @brief 可由应用外壳统一管理的业务模块接口。
 *
 * 模块生命周期按 initialize()、activate()/deactivate()、shutdown() 顺序管理；
 * rootWidget() 返回的界面由外壳放入页面栈，但其所有权仍由具体模块约定。
 */
class IApplicationModule
{
public:
    /** @brief 允许通过接口指针安全销毁具体模块。 */
    virtual ~IApplicationModule() = default;

    /** @return 模块用于页面切换的根窗口。 */
    virtual QWidget *rootWidget() = 0;

    /** @return 模块资源初始化成功时返回 true；重复调用应保持幂等。 */
    virtual bool initialize() = 0;

    /** @brief 使模块进入前台运行状态。 */
    virtual void activate() = 0;

    /** @brief 暂停模块的前台活动，但保留可再次激活所需资源。 */
    virtual void deactivate() = 0;

    /** @brief 终止模块并释放运行资源。 */
    virtual void shutdown() = 0;

    /** @return 模块当前处于前台活动状态时返回 true。 */
    virtual bool isActive() const = 0;

    /**
     * Returns true only when the module is on its unattended main screen.
     * Password entry, administration, menus, and secondary pages return false.
     */
    virtual bool allowsPresenceSwitch() const = 0;
};

#endif // IAPPLICATIONMODULE_H
