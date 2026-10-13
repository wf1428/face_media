<!--
 @file PersonnelImport.vue
 @brief 无目录要求的本地 Excel/人脸批次上传、封存、状态及部分成功反馈。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <van-cell-group inset title="导入本地人员">
    <van-cell title="选择 Excel 人员表" :label="excel?.name || batch?.result?.manifest?.find(file => file.kind === 'person_excel')?.name || '仅 .xls / .xlsx，使用原 U 盘模板'" is-link @click="excelInput.click()" />
    <input ref="excelInput" class="file-input" type="file" accept=".xls,.xlsx" :disabled="busy || !!batch" @change="selectExcel" />
    <van-cell title="选择人脸图片（可多选）" :label="`${batch?.result?.manifest?.filter(file => file.kind === 'person_face').length ?? images.length} 张，原名须为 账号_序号，例如 zhangsan_1.jpg`" is-link @click="imageInput.click()" />
    <input ref="imageInput" class="file-input" type="file" accept=".jpg,.jpeg,.png,.bmp" multiple :disabled="busy || !!batch" @change="selectImages" />
    <van-cell v-for="file in images" :key="file.name" :title="file.name" :value="formatSize(file.size)" />
    <van-cell v-if="batch?.state === 'OPEN'" v-for="file in batch?.result?.manifest || []" :key="'manifest-'+file.name" :title="file.name" :value="batch.assets?.some(asset => asset.name === file.name) ? '已上传' : '待上传'" />
    <div class="actions">
      <van-button round block type="primary" :disabled="!excel || busy || !!batch" :loading="busy" @click="upload">上传所选文件</van-button>
      <van-button v-if="batch?.state === 'OPEN' && uploaded" round block type="primary" :disabled="busy" @click="start">开始导入</van-button>
      <van-button v-if="batch" round block plain :disabled="busy" @click="refresh">刷新导入结果</van-button>
      <van-button v-if="batch && !busy && (terminal || batch.state === 'OPEN')" round block plain @click="reset">新建批次</van-button>
    </div>
    <van-cell v-if="progress" title="上传进度" :label="progress" />
    <van-cell v-if="batch" title="批次状态" :value="stateLabel" :label="batch.id" />
    <template v-if="batch?.result && batch.state !== 'OPEN'">
      <van-cell title="导入统计" :label="`新增 ${batch.result.inserted || 0}，覆盖 ${batch.result.updated || 0}，人脸入库 ${batch.result.savedFaces || 0}，失败 ${batch.result.failed || 0}`" />
      <van-cell v-if="batch.result.message" title="说明" :label="batch.result.message" />
      <van-cell v-for="(item, index) in batch.result.failedNames || []" :key="'f'+index" title="失败项" :label="item" />
      <van-cell v-for="(item, index) in batch.result.warnings || []" :key="'w'+index" title="文件提示" :label="item" />
    </template>
    <van-cell v-if="error" title="操作未完成" :label="error" />
    <van-button v-if="failedTask && !busy" block plain @click="cancelFailedTask">取消未完成上传任务</van-button>
  </van-cell-group>
</template>
<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue'
import { api, formatSize } from '../api'
import { showToast, showConfirmDialog } from 'vant'
const emit = defineEmits(['changed'])
const excel = ref(null), images = ref([]), batch = ref(null), busy = ref(false), uploaded = ref(false)
const excelInput = ref(null), imageInput = ref(null), progress = ref(''), error = ref('')
const failedTask = ref(sessionStorage.getItem('device-import-task') || '')
let timer = null // 仅导入进行中轮询；离页取消。
const terminalStates = ['DONE', 'PARTIAL_FAILED', 'FAILED', 'RECOVERY_REQUIRED']
const terminal = computed(() => terminalStates.includes(batch.value?.state))
const stateLabel = computed(() => ({ OPEN: '待开始导入', IMPORTING: '正在导入人员', VALIDATING_FACES: '校验人脸及防重', SAVING_FACES: '正在保存人脸', DONE: '完成', PARTIAL_FAILED: '部分失败，成功部分已保存', FAILED: '失败，请查看说明', RECOVERY_REQUIRED: '导入中断，须核对已入库人员，不能重放本批次' }[batch.value?.state] || batch.value?.state))
/** @brief 文件选择不要求目录；已上传批次不替换清单。 */
function selectExcel(event) {
  const file = event.target.files?.[0]; event.target.value = ''
  if (busy.value || batch.value) return
  if (!file || !/\.xlsx?$/i.test(file.name) || file.size > 20 * 1048576) { showToast('请选择不超过 20 MiB 的 Excel'); return }
  excel.value = file
}
/** @brief 保留图片名字与字节，不压缩改名，保持账号匹配规则。 */
function selectImages(event) {
  const files = Array.from(event.target.files || []); event.target.value = ''
  if (busy.value || batch.value) return
  if (files.length > 499 || files.some(file => !/\.(jpg|jpeg|png|bmp)$/i.test(file.name) || file.size > 10 * 1048576)) { showToast('图片格式/大小不符合要求，每张最多 10 MiB'); return }
  if (new Set(files.map(file => file.name.toLocaleLowerCase())).size !== files.length) { showToast('所选图片文件名重复'); return }
  images.value = files
}
/** @brief 所有文件顺序上传到一个批次，成功后才允许开始导入。 */
async function upload() {
  if (!excel.value || busy.value || batch.value) return
  busy.value = true; error.value = ''
  try {
    const files = [{ file: excel.value, kind: 'person_excel' }, ...images.value.map(file => ({ file, kind: 'person_face' }))]
    batch.value = await api.createImport(files.map(({ file, kind }) => ({ name: file.name, size: file.size, kind }))); sessionStorage.setItem('device-import-batch', batch.value.id)
    for (let i = 0; i < files.length; i++) await api.uploadFile(files[i].file, files[i].kind, batch.value.id, status => { progress.value = `${i + 1}/${files.length} · ${files[i].file.name} · ${status.percent}%` })
    await refresh()
    uploaded.value = true; progress.value = '所选文件上传完成，可以开始导入'
  } catch (failure) {
    failedTask.value = failure.taskId || ''; if (failedTask.value) sessionStorage.setItem('device-import-task', failedTask.value)
    error.value = `${failure.message}${failure.taskId ? '；任务：' + failure.taskId : ''}。失败批次不自动导入，请核对或新建批次。`
  }
  finally { busy.value = false }
}
/** @brief 封存后只执行一次；网络响应丢失只查询原批次。 */
async function start() {
  if (!batch.value || busy.value || !uploaded.value) return
  try { await showConfirmDialog({ title: '开始导入本地人员', message: '已有本地账号按原规则覆盖；成功部分保留，失败项列明。' }) } catch { return }
  busy.value = true; error.value = ''
  try { batch.value = await api.startImport(batch.value.id); schedule() } catch (failure) { error.value = failure.message } finally { busy.value = false }
}
/** @brief 仅工作阶段轮询，终止/离页停止。 */
function schedule() { clearTimeout(timer); if (batch.value && !terminal.value && batch.value.state !== 'OPEN') timer = setTimeout(refresh, 1500) }
/** @brief 查询真实结果，不把失败请求显示为成功。 */
async function refresh() {
  if (!batch.value) return
  try {
    batch.value = await api.importStatus(batch.value.id)
    const manifest = batch.value.result?.manifest || [], assets = batch.value.assets || []
    uploaded.value = manifest.length > 0 && manifest.every(file => assets.some(asset => asset.name === file.name && asset.kind === file.kind))
    if (terminal.value) emit('changed'); schedule()
  } catch (failure) { error.value = failure.message; clearTimeout(timer) }
}
/** @brief 新建页面批次不重复执行旧批次，保留服务端结果供核对。 */
async function reset() {
  if (batch.value?.state === 'OPEN') { try { await api.discardImport(batch.value.id) } catch (failure) { error.value = failure.message; return } }
  clearTimeout(timer); sessionStorage.removeItem('device-import-batch'); sessionStorage.removeItem('device-import-task'); failedTask.value = ''; batch.value = null; uploaded.value = false; excel.value = null; images.value = []; error.value = ''; progress.value = ''
}
/** @brief 释放断线后任务，活动写入者由服务端拒绝；已完成任务仅查询，不重传。 */
async function cancelFailedTask() {
  try {
    const task = await api.uploadStatus(failedTask.value)
    if (task.state !== 'READY') await api.cancelUpload(failedTask.value)
    failedTask.value = ''; sessionStorage.removeItem('device-import-task'); error.value = ''; await refresh()
  } catch (failure) { error.value = failure.message }
}
onMounted(async () => { const id = sessionStorage.getItem('device-import-batch'); if (id) { batch.value = { id }; await refresh() } })
onBeforeUnmount(() => clearTimeout(timer))
</script>
<style scoped>
.file-input { display: none; }
.actions { display: grid; gap: 10px; padding: var(--page-pad); }
</style>
