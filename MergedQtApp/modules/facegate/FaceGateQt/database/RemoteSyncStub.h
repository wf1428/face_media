/**
 * @file RemoteSyncStub.h
 * @brief 不执行网络操作的同步占位实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef REMOTE_SYNC_STUB_H
#define REMOTE_SYNC_STUB_H

#include "ISyncRepository.h"

#include <QtGlobal>

/**
 * @brief 不执行网络操作的同步占位实现。
 *
 * 所有方法均返回成功，用于在未接入服务器时保持本地业务与同步接口解耦。
 */
class RemoteSyncStub : public ISyncRepository {
public:
    /** @return 始终返回 true，不发送 person。 */
    bool pushPerson(const PersonInfo &person) override { Q_UNUSED(person); return true; }

    /** @return 始终返回 true，不发送 log。 */
    bool pushVerifyLog(const VerifyLog &log) override { Q_UNUSED(log); return true; }

    /** @return 始终返回 true，不拉取数据。 */
    bool pullRemotePersons() override { return true; }
};

#endif
