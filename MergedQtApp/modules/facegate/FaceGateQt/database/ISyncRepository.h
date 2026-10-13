/**
 * @file ISyncRepository.h
 * @brief 人员和验证日志远程同步的预留接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ISYNC_REPOSITORY_H
#define ISYNC_REPOSITORY_H

#include "VerificationTypes.h"

/** @brief 人员和验证日志远程同步的预留接口。 */
class ISyncRepository {
public:
    /** @brief 允许通过接口指针安全销毁实现。 */
    virtual ~ISyncRepository() = default;

    /** @brief 将人员数据推送到远端。 */
    virtual bool pushPerson(const PersonInfo &person) = 0;

    /** @brief 将验证日志推送到远端。 */
    virtual bool pushVerifyLog(const VerifyLog &log) = 0;

    /** @brief 从远端拉取人员变更。 */
    virtual bool pullRemotePersons() = 0;
};

#endif
