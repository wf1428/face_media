/**
 * @file local.js
 * @brief 标准设备本地 API、幂等键和异步任务轮询，不依赖 MQTT。
 * @author Du lin
 * @date 2026-10-16
 */

import { request } from './request'

/** @brief 生成本次本地写入使用的随机幂等键。 */
function key() {
  const bytes = new Uint8Array(16)
  crypto.getRandomValues(bytes)
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('')
}

// 只在内存保留结果未确定的请求键，网络失败后重试不会重新充值或新增。
const uncertainRequests = new Map()

/** @brief 发送标准本地 API 请求，并在结果不确定时复用幂等键。 */
export async function localRequest(method, path, data, options = {}) {
  const signature = method === 'get' ? '' : JSON.stringify([method, path, data])
  if (signature && !uncertainRequests.has(signature)) {
    if (uncertainRequests.size >= 32) throw new Error('有待核对的设备操作，请刷新实际状态后重试')
    uncertainRequests.set(signature, key())
  }

  // HTTP 明确成功或明确拒绝后释放键；连接失败及 5xx 保留原键。
  try {
    const result = await request(method, path, data, {
      baseURL: '/api',
      ...options,
      headers: {
        ...(signature ? { 'Idempotency-Key': uncertainRequests.get(signature) } : {}),
        ...options.headers,
      },
    })
    uncertainRequests.delete(signature)
    return result
  } catch (error) {
    if (error.status && error.status < 500) uncertainRequests.delete(signature)
    throw error
  }
}

/** @brief 轮询本地任务，只有 APPLIED 才返回实际业务结果。 */
export async function operation(data, timeout = 150000) {
  if (!data?.operationId) return data
  const deadline = Date.now() + timeout
  let current = data

  // 任务受理并不代表生效；以服务端持久化的最终状态为准。
  while (current.state === 'PENDING' && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 350))
    current = await localRequest('get', `/operations/${encodeURIComponent(current.operationId)}`)
  }
  if (current.state !== 'APPLIED') {
    const error = new Error(current.message || '设备操作尚未完成，请刷新核对实际状态')
    error.operationId = current.operationId
    throw error
  }
  return current.result
}
export const localApi = {
  /** @brief 读取已接入能力与本地硬件就绪状态。 */
  capabilities: () => localRequest('get', '/device/capabilities'),

  /** @brief 从 net_cfg.ini 读取保存的有线 IPv4 配置。 */
  getNetworkConfig: () => localRequest('get', '/device/network'),

  /** @brief 异步应用有线 DHCP/静态配置并查询实际执行、保存结果。 */
  saveNetworkConfig: async (data) =>
    operation(await localRequest('post', '/device/network', data), 70000),

  /** @brief 读取本地凭证统计、热点地址和实际媒体分区容量。 */
  deviceOverview: () => localRequest('get', '/device/overview'),

  /** @brief 请求当前界面截图，并等待异步采集结果。 */
  screenshot: async () => operation(await localRequest('post', '/device/screenshot', {}), 12000),

  /** @brief 读取真实播放器状态，不从请求参数推导播放结果。 */
  getPlayInfo: () => localRequest('post', '/device/getPlayInfo', {}),

  /** @brief 提交本地播放设置，并等待实际播放器状态确认。 */
  videoControl: async (data) =>
    operation(await localRequest('post', '/device/videoControl', data), 15000),

  /** @brief 逐页读取全部本地人员，保留服务端分页边界。 */
  personList: async () => {
    const people = []
    let offset = 0
    while (true) {
      const result = await localRequest(
        'get',
        '/person/list',
        { offset, limit: 500 },
        { envelope: true },
      )
      people.push(...result.data)
      if (!result.meta?.hasMore) return people
      if (!result.data.length) throw new Error('人员分页结果无效')
      offset += result.data.length
    }
  },

  /** @brief 并行读取人员基本资料与凭证聚合。 */
  personDetail: async ({ id }) => {
    const safe = encodeURIComponent(id)
    const [person, credential] = await Promise.all([
      localRequest('get', `/person/detail/${safe}`),
      localRequest('get', `/credential/getByPerson/${safe}`),
    ])
    return { person, credential }
  },

  /** @brief 新增本地人员，不调用网络人员同步入口。 */
  personAdd: (data) => localRequest('post', '/person/add', data),

  /** @brief 按人员版本差量更新基础资料。 */
  personUpdate: (data) => localRequest('put', '/person/update', data),

  /** @brief 软删除指定本地人员并使其权限失效。 */
  personDelete: (data) => localRequest('delete', '/person/delete', data),

  /** @brief 提交规则补丁及显式充值增量。 */
  credentialUpdate: (data) => localRequest('put', '/credential/update', data),

  /** @brief 录入单项凭证；人脸任务等待本地校验与实际入库。 */
  addSubItem: async (data) => operation(await localRequest('post', '/credential/addSubItem', data)),

  /** @brief 撤销指定卡、二维码或人脸子项。 */
  removeSubItem: (data) => localRequest('post', '/credential/removeSubItem', data),

  /** @brief 提交本地人员完整楼层集合。 */
  personFloorSave: (data) => localRequest('post', '/personFloor/save', data),

  /** @brief 查询本地人脸成功、失败和等待结果。 */
  faceRegisterResult: ({ id }) => localRequest('post', '/person/faceRegisterResult', { id }),

  /** @brief 读取设备保存的楼层开放和限行状态。 */
  floorLimitGet: () => localRequest('post', '/elevator/floorLimit/get', {}),

  /** @brief 读取当前设备定时规则及其可写能力。 */
  floorScheduleGet: () => localRequest('post', '/elevator/floorSchedule/get', {}),

  /** @brief 读取设备公共梯控特殊模式开关。 */
  elevatorModeGet: () => localRequest('post', '/elevator/elevatorMode/get', {}),

  /** @brief 将页面开放标签转换为标准动作值，并等待本地串口结果。 */
  floorLimitSet: async ({ floors }) =>
    operation(
      await localRequest('post', '/elevator/floorLimit/set', {
        floors: floors.map((f) => ({ floor: f.floor, action: f.status === 'open' ? 1 : 0 })),
      }),
      12000,
    ),

  /** @brief 提交完整模式开关，并等待本地发送结果。 */
  elevatorModeSet: async (mode) =>
    operation(await localRequest('post', '/elevator/elevatorMode/set', mode), 12000),

  /** @brief 登记立即重启任务，返回受理状态供页面提示。 */
  linuxReboot: () => localRequest('post', '/device/linux-reboot', {}),

  /** @brief 验证原登录密码并更新当前账号，成功后要求重新登录。 */
  changePassword: (data) => localRequest('post', '/device/change-password', data),
}
