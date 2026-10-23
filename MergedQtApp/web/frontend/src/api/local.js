/**
 * @file local.js
 * @brief 标准设备本地 API、幂等键和异步任务轮询，不依赖 MQTT。
 * @author Du lin
 * @date 2026-10-16
 */

import { request } from './request'

/**
 * @brief 生成本次本地写入使用的随机幂等键。
 * @returns {string} 16 字节安全随机数的十六进制文本，作为写入幂等键。
 */
function key() {
  const bytes = new Uint8Array(16)
  crypto.getRandomValues(bytes)
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('')
}

// 只在内存保留结果未确定的请求键，网络失败后重试不会重新充值或新增。
const uncertainRequests = new Map()

/**
 * @brief 发送标准本地 API 请求，并在结果不确定时复用幂等键。
 * @param {string} method HTTP 方法，小写 get、post、put 或 delete。
 * @param {string} path 标准本地 API 相对路径。
 * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
 * @param {object} options 可选请求、上传或绘制配置，未提供时使用默认值。
 * @returns {Promise<*>} 标准本地接口结果；网络失败或 5xx 保留幂等键供原请求重试。
 */
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

/**
 * @brief 轮询本地任务，只有 APPLIED 才返回实际业务结果。
 * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
 * @param {number} timeout 轮询总超时，单位为毫秒。
 * @returns {Promise<*>} 无任务号时原样返回数据；任务 APPLIED 时返回实际 result，超时或失败抛出错误。
 * @note 任务每 350 毫秒查询一次，PENDING 仅代表受理，APPLIED 才代表业务完成。
 */
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
  /**
   * @brief 读取已接入能力与本地硬件就绪状态。
   * @returns {Promise<*>} 设备实际能力和硬件就绪状态。请求失败时抛出错误。
   */
  capabilities: () => localRequest('get', '/device/capabilities'),

  /**
   * @brief 从 net_cfg.ini 读取保存的有线 IPv4 配置。
   * @returns {Promise<*>} 与后台共用的保存网络配置。请求失败时抛出错误。
   */
  getNetworkConfig: () => localRequest('get', '/device/network'),

  /**
   * @brief 异步应用有线 DHCP/静态配置并查询实际执行、保存结果。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 网络后台执行并持久化后的结果。请求失败时抛出错误。
   */
  saveNetworkConfig: async (data) =>
    operation(await localRequest('post', '/device/network', data), 70000),

  /**
   * @brief 读取本地凭证统计、热点地址和实际媒体分区容量。
   * @returns {Promise<*>} 实际人员、凭证和资源统计。请求失败时抛出错误。
   */
  deviceOverview: () => localRequest('get', '/device/overview'),

  /**
   * @brief 请求当前界面截图，并等待异步采集结果。
   * @returns {Promise<*>} 截图完成后的元数据。请求失败时抛出错误。
   */
  screenshot: async () => operation(await localRequest('post', '/device/screenshot', {}), 12000),

  /**
   * @brief 读取真实播放器状态，不从请求参数推导播放结果。
   * @returns {Promise<*>} 实际播放器快照及共用的广告类型、小图设置。请求失败时抛出错误。
   */
  getPlayInfo: () => localRequest('post', '/device/getPlayInfo', {}),

  /**
   * @brief 提交本地播放设置，并等待实际播放器状态确认。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 播放器实际应用后的结果。请求失败时抛出错误。
   */
  videoControl: async (data) =>
    operation(await localRequest('post', '/device/videoControl', data), 15000),

  /**
   * @brief 逐页读取全部本地人员，保留服务端分页边界。
   * @returns {Promise<*>} 全部分页合并的本地人员数组。请求失败时抛出错误。
   */
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

  /**
   * @brief 并行读取人员基本资料与凭证聚合。
   * @param {object} args 包含目标记录 id 的请求对象。
   * @param {string|number} args.id 目标记录标识。
   * @returns {Promise<*>} person 与 credential 聚合对象。请求失败时抛出错误。
   */
  personDetail: async ({ id }) => {
    const safe = encodeURIComponent(id)
    const [person, credential] = await Promise.all([
      localRequest('get', `/person/detail/${safe}`),
      localRequest('get', `/credential/getByPerson/${safe}`),
    ])
    return { person, credential }
  },

  /**
   * @brief 新增本地人员，不调用网络人员同步入口。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 新建人员结果。请求失败时抛出错误。
   */
  personAdd: (data) => localRequest('post', '/person/add', data),

  /**
   * @brief 按人员版本差量更新基础资料。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 按版本更新后的人员资料。请求失败时抛出错误。
   */
  personUpdate: (data) => localRequest('put', '/person/update', data),

  /**
   * @brief 软删除指定本地人员并使其权限失效。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 人员软删除结果。请求失败时抛出错误。
   */
  personDelete: (data) => localRequest('delete', '/person/delete', data),

  /**
   * @brief 提交规则补丁及显式充值增量。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 凭证规则及充值增量实际更新结果。请求失败时抛出错误。
   */
  credentialUpdate: (data) => localRequest('put', '/credential/update', data),

  /**
   * @brief 录入单项凭证；人脸任务等待本地校验与实际入库。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 新增凭证或人脸异步任务实际完成结果。请求失败时抛出错误。
   */
  addSubItem: async (data) => operation(await localRequest('post', '/credential/addSubItem', data)),

  /**
   * @brief 撤销指定卡、二维码或人脸子项。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 撤销指定凭证的结果。请求失败时抛出错误。
   */
  removeSubItem: (data) => localRequest('post', '/credential/removeSubItem', data),

  /**
   * @brief 提交本地人员完整楼层集合。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 保存后的完整楼层集合。请求失败时抛出错误。
   */
  personFloorSave: (data) => localRequest('post', '/personFloor/save', data),

  /**
   * @brief 查询本地人脸成功、失败和等待结果。
   * @param {object} args 包含目标记录 id 的请求对象。
   * @param {string|number} args.id 目标记录标识。
   * @returns {Promise<*>} 人员的人脸注册分项结果。请求失败时抛出错误。
   */
  faceRegisterResult: ({ id }) => localRequest('post', '/person/faceRegisterResult', { id }),

  /**
   * @brief 读取设备保存的楼层开放和限行状态。
   * @returns {Promise<*>} 共享保存的楼层开放状态。请求失败时抛出错误。
   */
  floorLimitGet: () => localRequest('post', '/elevator/floorLimit/get', {}),

  /**
   * @brief 读取当前设备定时规则及其可写能力。
   * @returns {Promise<*>} 共享定时规则和写入能力。请求失败时抛出错误。
   */
  floorScheduleGet: () => localRequest('post', '/elevator/floorSchedule/get', {}),
  /**
   * @brief 保存整份定时规则，并以读取快照防止覆盖其他入口的新修改。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 保存后的共享规则及确认状态。请求失败时抛出错误。
   */
  floorScheduleSet: async (data) => operation(await localRequest('post', '/elevator/floorSchedule/set', data), 12000),
  /**
   * @brief 按hash删除既有规则，使用独立 Web 执行器及共享规则存储。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 删除指定 hash 后的共享规则。请求失败时抛出错误。
   */
  floorScheduleDelete: async (data) => operation(await localRequest('post', '/elevator/floorSchedule/delete', data), 12000),

  /**
   * @brief 读取设备公共梯控特殊模式开关。
   * @returns {Promise<*>} 与 MQTT 共用的特殊模式状态。请求失败时抛出错误。
   */
  elevatorModeGet: () => localRequest('post', '/elevator/elevatorMode/get', {}),

  /**
   * @brief 将页面开放标签转换为标准动作值，并等待本地串口结果。
   * @param {object} args 完整楼层状态数组，status=open 表示开放。
   * @param {object[]} args.floors 楼层开放状态条目。
   * @returns {Promise<*>} 楼层配置保存及串口发送结果。请求失败时抛出错误。
   */
  floorLimitSet: async ({ floors }) =>
    operation(
      await localRequest('post', '/elevator/floorLimit/set', {
        floors: floors.map((f) => ({ floor: f.floor, action: f.status === 'open' ? 1 : 0 })),
      }),
      12000,
    ),

  /**
   * @brief 提交完整模式开关，并等待本地发送结果。
   * @param {object} mode 完整主开关与八个特殊模式开关，字段与共享模式配置一致。
   * @returns {Promise<*>} 模式保存及串口发送结果。请求失败时抛出错误。
   */
  elevatorModeSet: async (mode) =>
    operation(await localRequest('post', '/elevator/elevatorMode/set', mode), 12000),

  /**
   * @brief 登记立即重启任务，返回受理状态供页面提示。
   * @returns {Promise<*>} 重启请求的受理结果。请求失败时抛出错误。
   */
  linuxReboot: () => localRequest('post', '/device/linux-reboot', {}),

  /**
   * @brief 验证原登录密码并更新当前账号，成功后要求重新登录。
   * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
   * @returns {Promise<*>} 登录密码更新结果。请求失败时抛出错误。
   */
  changePassword: (data) => localRequest('post', '/device/change-password', data),
}
