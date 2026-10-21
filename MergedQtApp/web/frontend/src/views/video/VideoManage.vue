<!--
 @file VideoManage.vue
 @brief 保留视频页，接入真实文件上传、下载、删除；播放设置通过本地业务执行。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('video.title') }}</div>
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
        <van-radio-group v-model="playSource" direction="horizontal" class="source-options">
          <van-radio name="file">{{ $t('video.localFile') }}</van-radio>
          <van-radio name="url">{{ $t('video.networkUrl') }}</van-radio>
          <van-radio name="none">{{ $t('video.stopPlay') }}</van-radio>
        </van-radio-group>
      </div>
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
    <van-cell-group inset :title="$t('video.uploadVideo')">
      <input
        ref="fileRef"
        type="file"
        accept=".mp4,.avi,.mov,.mkv,.ts,.webm,.m4v"
        class="file-input"
        :disabled="uploading"
        @change="onFileChange"
      />
      <van-cell
        :title="pendingFile ? pendingFile.name : $t('video.selectVideo')"
        :label="pendingFile ? formatSize(pendingFile.size) : $t('video.videoHint')"
        icon="video-o"
        is-link
        @click="pickFile"
      />
      <div v-if="pendingFile" class="pending-bar">
        <van-button round block type="primary" :loading="uploading" @click="onUpload">
          {{ $t('common.uploadToDevice') }}
        </van-button>
      </div>
    </van-cell-group>

    <van-cell v-if="uploadProgress" title="上传进度" :label="uploadProgress" />
    <van-cell v-if="failedTask" title="未完成任务" :label="failedTask" />
    <van-button v-if="failedTask && !uploading" block plain @click="resolveTask"
      >查询并取消未完成任务</van-button
    >

    <div class="footer">
      <van-button type="primary" block round :loading="saving" @click="saveSettings">{{
        $t('video.savePlaySettings')
      }}</van-button>
    </div>

    <!-- 视频 / 广告列表 -->
    <van-cell-group inset :title="$t('video.listTitle', { n: files.length })">
      <div v-for="f in files" :key="f.id" class="file-row">
        <van-cell :title="f.name" :label="`${f.size} · ${f.duration}`" icon="video-o" />
        <van-button size="small" round type="danger" plain class="del-btn" @click="onDelete(f)">
          {{ $t('common.delete') }}
        </van-button>
        <a class="download" :href="api.downloadUrl(f.id)">下载</a>
      </div>
      <van-empty v-if="!files.length" :description="$t('video.noVideo')" />
    </van-cell-group>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'

const { t } = useI18n()

const files = ref([])
const pendingFile = ref(null)
const uploading = ref(false)
const saving = ref(false)
const fileRef = ref(null)
const uploadProgress = ref('')
const failedTask = ref(sessionStorage.getItem('device-video-task') || '')

const play = reactive({ name: '', videoType: '', videoVolume: null, videoPath: '', videoUrl: '' })
const playSource = ref('file')
const videoUrl = ref('')
const volume = ref(50)

/** @brief 播放类型文案：本地文件 / 网络链接 / 停止播放 */
function typeText(type) {
  const keyMap = { RECORDED: 'video.localFile', LIVE: 'video.networkUrl', NONE: 'video.stopPlay' }
  return keyMap[type] ? t(keyMap[type]) : type || '-'
}

/** @brief 读取真实播放器快照，刷新当前模式、音量和播放目标。 */
async function loadPlayInfo() {
  try {
    const p = await api.getPlayInfo()
    Object.assign(play, p)
    videoUrl.value = p.videoUrl || ''
    volume.value = p.videoVolume != null ? p.videoVolume : 50
    if (p.videoType === 'LIVE') playSource.value = 'url'
    else if (p.videoType === 'NONE') playSource.value = 'none'
    else playSource.value = 'file'
  } catch (e) {
    showToast(e.message)
  }
}

/** @brief 读取并刷新设备视频文件清单。 */
async function load() {
  try {
    files.value = await api.playFileList()
    await loadPlayInfo()
  } catch (error) {
    showToast(error.message)
  }
}

/** @brief 在未上传时打开本地视频文件选择器。 */
function pickFile() {
  if (uploading.value) return
  fileRef.value && fileRef.value.click()
}

/** @brief 记录用户选中的原始视频文件。 */
function onFileChange(e) {
  const f = e.target.files && e.target.files[0]
  if (f) pendingFile.value = f
  e.target.value = ''
}

/** @brief 将视频字节数转换为页面展示文本。 */
function formatSize(bytes) {
  if (!bytes && bytes !== 0) return '-'
  const mb = bytes / 1024 / 1024
  if (mb >= 1) return mb.toFixed(1) + 'MB'
  return Math.max(1, Math.round(bytes / 1024)) + 'KB'
}

/** @brief 上传选中的视频并保留失败任务供核对。 */
async function onUpload() {
  if (!pendingFile.value) {
    showToast(t('video.selectVideoFirst'))
    return
  }
  uploading.value = true
  try {
    // 发送浏览器选中的原始视频文件，由设备端校验后发布。
    await api.uploadVideo(pendingFile.value, (status) => {
      uploadProgress.value = `${status.percent}% · ${status.state === 'VALIDATING' ? '设备正在校验' : status.state === 'READY' ? '已完成' : '正在传输'}`
    })
    pendingFile.value = null
    showToast({ type: 'success', message: t('video.uploadSuccess') })
    load()
  } catch (e) {
    // 保存原上传任务编号，网络超时后可以查询状态而不重复上传。
    failedTask.value = e.taskId || ''
    if (failedTask.value) sessionStorage.setItem('device-video-task', failedTask.value)
    showToast({ type: 'fail', message: e.message || t('common.uploadFailed') })
  } finally {
    uploading.value = false
  }
}

/** @brief 查询原任务并释放未发布缓存，禁止响应丢失后重复创建同名视频。 */
async function resolveTask() {
  try {
    const task = await api.uploadStatus(failedTask.value)
    if (task.state !== 'READY') await api.cancelUpload(failedTask.value)
    failedTask.value = ''
    sessionStorage.removeItem('device-video-task')
    await load()
  } catch (error) {
    showToast(error.message)
  }
}

/** @brief 确认后删除设备视频并刷新清单。 */
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

/** @brief 通过本地播放器入口应用设置并查询执行结果。 */
async function saveSettings() {
  let videoType = 'RECORDED'
  if (playSource.value === 'url') videoType = 'LIVE'
  else if (playSource.value === 'none') videoType = 'NONE'

  // 本地文件来源才携带 videoPath；待上传文件名优先
  const file = playSource.value === 'file' && pendingFile.value ? pendingFile.value.name : ''
  const videoPath = playSource.value === 'file' ? file || play.videoPath || '' : ''

  saving.value = true
  try {
    await api.videoControl({
      videoType,
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
