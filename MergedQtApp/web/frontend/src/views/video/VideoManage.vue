<!--
 @file VideoManage.vue
 @brief 保留视频页，接入真实文件上传、下载、删除；播放设置保持禁用。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('video.title') }}</div>
    </div>

    <UnsupportedPanel>
    <!-- 当前播放信息 -->
    <van-cell-group inset>
      <template #title>
        <div class="group-head">
          <span>{{ $t('video.currentPlay') }}</span>
          <van-button size="mini" plain type="primary" disabled>{{ $t('common.refresh') }}</van-button>
        </div>
      </template>
      <van-cell :title="$t('video.adName')" :value="play.name || '-'" />
      <van-cell :title="$t('video.type')" :value="typeText(play.videoType)" />
      <van-cell :title="$t('video.volume')" :value="play.videoVolume != null ? play.videoVolume + '%' : '-'" />
      <van-cell :title="$t('video.filePath')" :value="play.videoPath || '-'" />
      <van-cell :title="$t('video.url')" :value="play.videoUrl || '-'" />
    </van-cell-group>

    <!-- 播放设置 -->
    <van-cell-group inset :title="$t('video.playSettings')">
      <van-cell>
        <template #title><span>{{ $t('video.playSource') }}</span></template>
        <van-radio-group v-model="playSource" direction="horizontal">
          <van-radio name="file">{{ $t('video.localFile') }}</van-radio>
          <van-radio name="url">{{ $t('video.networkUrl') }}</van-radio>
          <van-radio name="none">{{ $t('video.stopPlay') }}</van-radio>
        </van-radio-group>
      </van-cell>
      <van-field v-model="videoUrl" :label="$t('video.videoUrl')" :placeholder="$t('video.urlPlaceholder')" clearable />
      <div class="volume-row">
        <span class="volume-label">{{ $t('video.volumeN', { n: volume }) }}</span>
        <van-slider v-model="volume" :min="0" :max="100" :step="1" />
      </div>
    </van-cell-group>

    </UnsupportedPanel>
    <!-- 上传 -->
    <van-cell-group inset :title="$t('video.uploadVideo')">
      <input ref="fileRef" type="file" accept=".mp4,.avi,.mov,.mkv,.ts,.webm,.m4v" class="file-input" :disabled="uploading" @change="onFileChange" />
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
    <van-button v-if="failedTask && !uploading" block plain @click="resolveTask">查询并取消未完成任务</van-button>
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

    <div class="footer">
      <van-button type="primary" block round disabled>{{ $t('video.savePlaySettings') }}（本版本未接入）</van-button>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'
import UnsupportedPanel from '../../components/UnsupportedPanel.vue'

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

/** 播放类型文案：本地文件 / 网络链接 / 停止播放 */
function typeText(type) {
  const keyMap = { RECORDED: 'video.localFile', LIVE: 'video.networkUrl', NONE: 'video.stopPlay' }
  return keyMap[type] ? t(keyMap[type]) : (type || '-')
}

async function loadPlayInfo() { /* 原控件保留，本版本不执行该业务。 */ }

async function load() {
  try { files.value = await api.playFileList() } catch (error) { showToast(error.message) }
}

function pickFile() {
  if (uploading.value) return
  fileRef.value && fileRef.value.click()
}

function onFileChange(e) {
  const f = e.target.files && e.target.files[0]
  if (f) pendingFile.value = f
  e.target.value = ''
}

function formatSize(bytes) {
  if (!bytes && bytes !== 0) return '-'
  const mb = bytes / 1024 / 1024
  if (mb >= 1) return mb.toFixed(1) + 'MB'
  return Math.max(1, Math.round(bytes / 1024)) + 'KB'
}

async function onUpload() {
  if (!pendingFile.value) {
    showToast(t('video.selectVideoFirst'))
    return
  }
  uploading.value = true
  try {
    await api.uploadVideo(pendingFile.value, status => { uploadProgress.value = `${status.percent}% · ${status.state === 'VALIDATING' ? '设备正在校验' : status.state === 'READY' ? '已完成' : '正在传输'}` })
    pendingFile.value = null
    showToast({ type: 'success', message: t('video.uploadSuccess') })
    load()
  } catch (e) {
    failedTask.value = e.taskId || ''; if (failedTask.value) sessionStorage.setItem('device-video-task', failedTask.value)
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
    failedTask.value = ''; sessionStorage.removeItem('device-video-task'); await load()
  } catch (error) { showToast(error.message) }
}

async function onDelete(f) {
  try { await showConfirmDialog({ title: t('video.deleteVideo'), message: t('common.deleteConfirm', { name: f.name }) }) } catch { return }
  try { await api.deletePlayFile({ id: f.id }); showToast({ type: 'success', message: t('common.deleted') }); await load() } catch (error) { showToast(error.message) }
}

async function saveSettings() { /* 原控件保留，本版本不执行该业务。 */ }

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
  padding: 8px var(--page-pad);
}
.volume-label {
  font-size: 13px;
  color: var(--text-2);
}
.file-row {
  position: relative;
  padding-right: 140px;
}
.download { position: absolute; right: 90px; top: 50%; transform: translateY(-50%); color: var(--primary); }
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
