<!--
 @file VideoManage.vue
 @brief 保留视频页，接入真实文件上传、下载、删除；播放设置通过本地业务执行。
 @author Du lin
 @date 2026-10-09
 @note 最近修改：2026-10-23；修改人：Du lin；移除队列摘要下的数量、大小和空间检查提示文案，保留队列摘要、上传限制及全部校验逻辑。
-->

<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('video.mediaTitle') }}</div>
    </div>

    <!-- 当前播放信息 -->
    <van-cell-group inset>
      <template #title>
        <div class="group-head">
          <span>{{ $t('video.currentPlay') }}</span>
          <van-button size="mini" plain type="primary" @click="loadPlayInfo">{{
            $t('common.refresh')
          }}</van-button>
        </div>
      </template>
      <van-cell :title="$t('video.adName')" :value="play.name || '-'" />
      <van-cell :title="$t('video.type')" :value="typeText(play.videoType)" />
      <van-cell
        :title="$t('video.volume')"
        :value="play.videoVolume != null ? play.videoVolume + '%' : '-'"
      />
      <van-cell :title="$t('video.filePath')" :value="play.videoPath || '-'" />
      <van-cell :title="$t('video.url')" :value="play.videoUrl || '-'" />
    </van-cell-group>

    <!-- 播放设置 -->
    <van-cell-group inset :title="$t('video.playSettings')" class="play-settings">
      <div class="source-row">
        <div class="source-label">{{ $t('video.playSource') }}</div>
        <van-radio-group v-model="playSource" direction="horizontal" class="source-options" :disabled="busy">
          <van-radio name="file">{{ $t('video.localFile') }}</van-radio>
          <van-radio name="url">{{ $t('video.networkUrl') }}</van-radio>
          <van-radio name="none">{{ $t('video.stopPlay') }}</van-radio>
        </van-radio-group>
      </div>
      <div v-if="playSource === 'file'" class="source-row content-type-row">
        <div class="source-label">{{ $t('video.contentType') }}</div>
        <van-radio-group v-model="contentType" direction="horizontal" class="source-options" :disabled="busy">
          <van-radio name="VIDEO">{{ $t('video.contentVideo') }}</van-radio>
          <van-radio name="IMAGE">{{ $t('video.contentImage') }}</van-radio>
          <van-radio name="VIDEO_IMAGE">{{ $t('video.contentMixed') }}</van-radio>
        </van-radio-group>
      </div>
      <van-cell v-if="showSmallImage" :title="$t('video.smallImageMode')" :label="$t('video.smallImageHint')" center>
        <template #right-icon><van-switch v-model="smallImageMode" size="22" :disabled="busy" /></template>
      </van-cell>
      <van-field
        v-model="videoUrl"
        :label="$t('video.videoUrl')"
        :placeholder="$t('video.urlPlaceholder')"
        clearable
      />
      <div class="volume-row">
        <span class="volume-label">{{ $t('video.volumeN', { n: volume }) }}</span>
        <van-slider v-model="volume" :min="0" :max="100" :step="1" />
      </div>
    </van-cell-group>

    <!-- 上传 -->
    <van-cell-group inset :title="$t('video.uploadMedia')">
      <input
        ref="fileRef"
        type="file"
        multiple
        accept=".mp4,.avi,.mov,.mkv,.ts,.webm,.m4v,.png,.jpg,.jpeg,.bmp,.gif,.webp,.tif,.tiff"
        class="file-input"
        :disabled="busy"
        @change="onFileChange"
      />
      <van-cell
        :title="$t('video.selectMedia')"
        :label="$t('video.mediaHint')"
        icon="video-o"
        is-link
        :disabled="busy || pendingFiles.length >= MEDIA_UPLOAD_LIMITS.files"
        @click="pickFile"
      />
      <van-cell
        :title="$t('video.queueSummary', { count: pendingFiles.length, max: MEDIA_UPLOAD_LIMITS.files, size: formatSize(queueBytes) })"
      />
      <div v-for="entry in pendingFiles" :key="entry.key" class="upload-queue-row">
        <van-cell
          :title="entry.file.name"
          :label="`${formatSize(entry.file.size)} · ${queueStateText(entry)}${entry.error ? ' · ' + entry.error : ''}`"
          :icon="mediaFileKind(entry.file.name) === 'ad_image' ? 'photo-o' : 'video-o'"
        />
        <button type="button" class="queue-remove" :disabled="busy"
          :aria-label="$t('video.queueRemove', { name: entry.file.name })" @click="removePendingFile(entry)">
          <van-icon name="cross" />
        </button>
      </div>
      <div v-if="pendingFiles.length" class="pending-bar">
        <van-button round block type="primary" :loading="uploading" :disabled="saving || resolvingTask || !!failedTask" @click="onUpload">
          {{ $t('common.uploadToDevice') }}
        </van-button>
      </div>
    </van-cell-group>

    <van-cell v-if="uploadProgress" title="上传进度" :label="uploadProgress" />
    <van-cell v-if="failedTask" title="未完成任务" :label="failedTask" />
    <van-button v-if="failedTask && !uploading" block plain :loading="resolvingTask" :disabled="saving" @click="resolveTask"
      >查询并取消未完成任务</van-button
    >

    <div class="footer">
      <van-button type="primary" block round :loading="saving" :disabled="uploading || resolvingTask" @click="saveSettings">{{
        $t('video.savePlaySettings')
      }}</van-button>
    </div>

    <!-- 视频 / 广告列表 -->
    <van-cell-group inset :title="$t('video.mediaListTitle', { n: files.length })">
      <div v-for="f in files" :key="f.id" class="file-row">
        <van-cell :title="f.name" :label="`${f.size} · ${f.smallImageMode ? $t('video.picRegion') : $t('video.mainRegion')}`"
          :icon="f.mediaType === 'IMAGE' ? 'photo-o' : 'video-o'" />
        <van-button size="small" round type="danger" plain class="del-btn" @click="onDelete(f)">
          {{ $t('common.delete') }}
        </van-button>
        <a class="download" :href="api.downloadUrl(f.id)">下载</a>
      </div>
      <van-empty v-if="!files.length" :description="$t('video.noMedia')" />
    </van-cell-group>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'
import { MEDIA_UPLOAD_LIMITS, mediaFileKind, mergeMediaSelection } from '../../utils/mediaUploadQueue'

const { t } = useI18n()

const files = ref([])
/** @description 只保存浏览器 File 引用及任务状态，不预读视频或图片内容。 */
const pendingFiles = ref([])
const uploading = ref(false)
const saving = ref(false)
const fileRef = ref(null)
const uploadProgress = ref('')
const failedTask = ref(sessionStorage.getItem('device-video-task') || '')
const resolvingTask = ref(false)
const busy = computed(() => uploading.value || saving.value || resolvingTask.value)
const queueBytes = computed(() => pendingFiles.value.reduce((sum, entry) => sum + entry.file.size, 0))
let queueSerial = 0

const play = reactive({ name: '', videoType: '', videoVolume: null, videoPath: '', videoUrl: '' })
const playSource = ref('file')
const contentType = ref('VIDEO_IMAGE')
const smallImageMode = ref(false)
/** @description 仅本地来源且队列含图片时显示小图选项；视频不受该保存目录选项影响。 */
const showSmallImage = computed(() =>
  playSource.value === 'file' && pendingFiles.value.some((entry) => mediaFileKind(entry.file.name) === 'ad_image')
)
const videoUrl = ref('')
const volume = ref(50)

/**
 * @brief 播放类型文案：本地文件 / 网络链接 / 停止播放
 * @param {string} type 设备播放器返回的类型值。
 * @returns {string} 本地、网络或停止播放的当前语言标签；未知值保留原文。
 */
function typeText(type) {
  const keyMap = { RECORDED: 'video.localFile', LIVE: 'video.networkUrl', NONE: 'video.stopPlay' }
  return keyMap[type] ? t(keyMap[type]) : type || '-'
}

/**
 * @brief 读取真实播放器快照，刷新当前模式、音量和播放目标。
 */
async function loadPlayInfo() {
  try {
    const p = await api.getPlayInfo()
    Object.assign(play, p)
    contentType.value = ['VIDEO', 'IMAGE'].includes(p.contentType) ? p.contentType : 'VIDEO_IMAGE'
    smallImageMode.value = Number(p.smallImageMode) === 1
    videoUrl.value = p.videoUrl || ''
    volume.value = p.videoVolume != null ? p.videoVolume : 50
    if (p.videoType === 'LIVE') playSource.value = 'url'
    else if (p.videoType === 'NONE') playSource.value = 'none'
    else playSource.value = 'file'
  } catch (e) {
    showToast(e.message)
  }
}

/**
 * @brief 读取并刷新 video 目录的视频及图片清单。
 */
async function load() {
  try {
    files.value = await api.playFileList()
    await loadPlayInfo()
  } catch (error) {
    showToast(error.message)
  }
}

/**
 * @brief 在未上传时打开本地媒体文件选择器。
 */
function pickFile() {
  if (busy.value || pendingFiles.value.length >= MEDIA_UPLOAD_LIMITS.files) return
  fileRef.value && fileRef.value.click()
}

/**
 * 合并多次文件选择，超限时整次拒绝新增，保持已有队列及原始文件不变。
 * @param {Event} e 文件输入控件的变更事件。
 */
function onFileChange(e) {
  const selected = Array.from(e.target.files || [])
  e.target.value = ''
  if (busy.value || !selected.length) return
  const result = mergeMediaSelection(pendingFiles.value.map((entry) => entry.file), selected)
  if (result.error) {
    showToast(t(result.error.key, result.error.params))
    return
  }
  for (const file of result.files.slice(pendingFiles.value.length))
    pendingFiles.value.push({ key: ++queueSerial, file, state: 'WAITING', percent: 0, error: '', taskId: '' })
  if (result.skipped) showToast(t('video.queueSkipped', { count: result.skipped }))
}

/**
 * 返回当前队列项的本地化状态，进度以服务端发布结果为准。
 * @param {object} entry 含 state 和 percent 的队列项。
 * @returns {string} 等待、传输、校验或失败提示。
 */
function queueStateText(entry) {
  const key = { WAITING: 'queueWaiting', CREATED: 'queueWaiting', RECEIVING: 'queueReceiving',
    VALIDATING: 'queueValidating', READY: 'queueReady', FAILED: 'queueFailed' }[entry.state] || 'queueWaiting'
  return t(`video.${key}`) + (entry.state === 'WAITING' ? '' : ` ${entry.percent}%`)
}

/**
 * 记录唯一在途任务；刷新后只核对原任务，不盲目重传可能已发布的文件。
 * @param {string} id 服务端任务号；空值清除已完成或已取消任务。
 */
function rememberTask(id) {
  failedTask.value = id
  try {
    if (id) sessionStorage.setItem('device-video-task', id)
    else sessionStorage.removeItem('device-video-task')
  } catch {
    // 浏览器禁用会话存储时，当前页面仍保留任务号并完成上传。
  }
}

/**
 * 从待上传队列移除一项；已创建任务的失败项先核对或取消服务端任务。
 * @param {object} entry 用户点击删除的队列项。
 * @returns {Promise<void>} 无在途任务或原任务处理成功后移除，否则保留该项供重试核对。
 */
async function removePendingFile(entry) {
  if (busy.value) return
  if (entry.taskId && entry.taskId === failedTask.value && !(await resolveTask())) return
  pendingFiles.value = pendingFiles.value.filter((item) => item.key !== entry.key)
}

/**
 * @brief 将媒体字节数转换为页面展示文本。
 * @param {number} bytes 文件长度，单位为字节。
 * @returns {string} 容量展示文本；无效值按当前页面约定显示占位符。
 */
function formatSize(bytes) {
  if (!bytes && bytes !== 0) return '-'
  if (bytes === 0) return '0B'
  if (bytes >= 1024 * 1024 * 1024) return (bytes / 1024 / 1024 / 1024).toFixed(2) + 'GB'
  const mb = bytes / 1024 / 1024
  if (mb >= 1) return mb.toFixed(1) + 'MB'
  return Math.max(1, Math.round(bytes / 1024)) + 'KB'
}

/**
 * 依队列顺序逐项上传并发布，失败时停止，保留失败项和后续未上传项。
 * @returns {Promise<void>} 已完成项从队列移除；未确定任务必须先查询再重试。
 * 使用批次开始时的播放类型与小图选项，沿用设备端单媒体任务和实际空间保护。
 */
async function onUpload() {
  if (busy.value) return
  if (failedTask.value) {
    showToast(t('video.queueResolveFirst'))
    return
  }
  if (!pendingFiles.value.length) {
    showToast(t('video.queueSelectFirst'))
    return
  }
  const batch = [...pendingFiles.value]
  const checked = mergeMediaSelection([], batch.map((entry) => entry.file))
  if (checked.error) {
    showToast(t(checked.error.key, checked.error.params))
    return
  }
  const selectedContent = contentType.value
  const selectedSmall = smallImageMode.value ? 1 : 0
  let activeEntry = null
  uploading.value = true
  try {
    await api.videoControl({ videoType: 'RECORDED', contentType: selectedContent, smallImageMode: selectedSmall })
    for (const [index, entry] of batch.entries()) {
      activeEntry = entry
      entry.state = 'RECEIVING'
      entry.percent = 0
      entry.error = ''
      await api.uploadMedia(entry.file, (status) => {
        if (entry.state === 'READY' || entry.state === 'FAILED') return
        entry.state = status.state
        entry.percent = status.percent
        uploadProgress.value = `${index + 1}/${batch.length} · ${entry.file.name} · ${queueStateText(entry)}`
      }, {
        smallImageMode: selectedSmall,
        /** @param {object} task 已创建的单文件任务，先保留 id 再发送文件字节。 */
        onTaskCreated: (task) => { entry.taskId = task.id; rememberTask(task.id) },
      })
      entry.state = 'READY'
      entry.percent = 100
      uploadProgress.value = `${index + 1}/${batch.length} · ${entry.file.name} · ${queueStateText(entry)}`
      pendingFiles.value = pendingFiles.value.filter((item) => item.key !== entry.key)
      rememberTask('')
      activeEntry = null
    }
    showToast({ type: 'success', message: t('video.mediaUploadSuccess') })
  } catch (e) {
    if (activeEntry) {
      activeEntry.state = 'FAILED'
      activeEntry.error = e.message || t('common.uploadFailed')
      activeEntry.taskId = e.taskId || activeEntry.taskId
      rememberTask(activeEntry.taskId)
    }
    showToast({ type: 'fail', message: e.message || t('common.uploadFailed') })
  } finally {
    await load()
    uploading.value = false
  }
}

/**
 * 查询原任务并释放未发布缓存；已 READY 的队列项直接移除，避免重复上传。
 * @returns {Promise<boolean>} 原任务完成核对或成功取消时为 true，否则保留任务号并返回 false。
 */
async function resolveTask() {
  if (busy.value || !failedTask.value) return false
  resolvingTask.value = true
  try {
    const id = failedTask.value
    const task = await api.uploadStatus(id)
    if (task.state !== 'READY') await api.cancelUpload(id)
    if (task.state === 'READY') pendingFiles.value = pendingFiles.value.filter((entry) => entry.taskId !== id)
    else for (const entry of pendingFiles.value.filter((item) => item.taskId === id))
      Object.assign(entry, { state: 'WAITING', percent: 0, error: '', taskId: '' })
    rememberTask('')
    await load()
    return true
  } catch (error) {
    showToast(error.message)
    return false
  } finally {
    resolvingTask.value = false
  }
}

/**
 * @brief 确认后删除指定视频或图片并刷新清单。
 * @param {object} f 待删除的媒体条目，包含服务端 id 和展示名称。
 */
async function onDelete(f) {
  try {
    await showConfirmDialog({
      title: t('video.deleteVideo'),
      message: t('common.deleteConfirm', { name: f.name }),
    })
  } catch {
    return
  }
  try {
    await api.deletePlayFile({ id: f.id })
    showToast({ type: 'success', message: t('common.deleted') })
    await load()
  } catch (error) {
    showToast(error.message)
  }
}

/**
 * @brief 通过本地播放器入口应用设置并查询执行结果。
 */
async function saveSettings() {
  if (busy.value) return
  let videoType = 'RECORDED'
  if (playSource.value === 'url') videoType = 'LIVE'
  else if (playSource.value === 'none') videoType = 'NONE'

  // 本地文件来源才携带 videoPath；待上传文件名优先
  const file = playSource.value === 'file' ? pendingFiles.value[0]?.file.name || '' : ''
  const videoPath = playSource.value === 'file' ? file || play.videoPath || '' : ''

  saving.value = true
  try {
    await api.videoControl({
      videoType,
      contentType: contentType.value,
      smallImageMode: smallImageMode.value ? 1 : 0,
      videoVolume: volume.value,
      videoUrl: playSource.value === 'url' ? videoUrl.value : '',
      videoPath,
      ...(file ? { file } : {}),
    })
    showToast({ type: 'success', message: t('common.saved') })
    await loadPlayInfo()
  } catch (e) {
    showToast({ type: 'fail', message: e.message || t('common.saveFailed') })
  } finally {
    saving.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.page {
  padding-top: var(--page-pad);
  padding-bottom: 80px;
}
.header {
  padding: 0 var(--page-pad) var(--gap);
}
.title {
  font-size: 20px;
  font-weight: 600;
}
.group-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.file-input {
  display: none;
}
.pending-bar {
  padding: var(--page-pad);
}
.upload-queue-row {
  display: flex;
  align-items: center;
}
.upload-queue-row :deep(.van-cell) {
  flex: 1;
  min-width: 0;
}
.upload-queue-row :deep(.van-cell__title) {
  min-width: 0;
  overflow-wrap: anywhere;
}
.queue-remove {
  flex-shrink: 0;
  margin-right: 12px;
  padding: 10px;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: 18px;
  cursor: pointer;
}
.queue-remove:disabled {
  opacity: 0.4;
  cursor: default;
}
.volume-row {
  padding: 12px 26px 30px;
}
.play-settings {
  padding-bottom: 8px;
}
.source-row {
  padding: 14px 16px;
}
.source-label {
  margin-bottom: 12px;
  font-size: 14px;
  color: var(--text-1);
}
.source-options {
  display: flex;
  flex-wrap: wrap;
  gap: 12px 18px;
}
.source-options :deep(.van-radio) {
  margin: 0;
  min-height: 26px;
}
.source-options :deep(.van-radio__label) {
  white-space: nowrap;
}
.volume-label {
  display: block;
  margin-bottom: 18px;
  font-size: 13px;
  color: var(--text-2);
}
.file-row {
  position: relative;
  padding-right: 140px;
}
.download {
  position: absolute;
  right: 90px;
  top: 50%;
  transform: translateY(-50%);
  color: var(--primary);
}
.del-btn {
  position: absolute;
  right: 12px;
  top: 50%;
  transform: translateY(-50%);
}
.footer {
  padding: var(--page-pad);
}
</style>
