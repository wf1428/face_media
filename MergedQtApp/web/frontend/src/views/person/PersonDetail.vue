<!-- @file PersonDetail.vue | 本地人员与标准分项凭证接口；不发送 MQTT。 -->
<template>
  <div class="page">
    <van-overlay :show="busy || saving || savingPerson" :z-index="3000"
      ><div class="local-busy">设备正在处理，请稍候</div></van-overlay
    >
    <van-nav-bar
      :title="$t('person.detailTitle')"
      left-arrow
      :right-text="$t('common.delete')"
      @click-left="router.back()"
      @click-right="onDeletePerson"
    />

    <!-- 人员基本信息 -->
    <div v-if="person" class="person-card">
      <div class="person-head">
        <div class="person-name">{{ person.name }}</div>
        <van-button size="mini" round plain type="primary" @click="openEditPerson">{{
          $t('common.edit')
        }}</van-button>
      </div>
      <div class="person-meta">
        <span v-if="person.employeeNo">{{ $t('person.employeeNo') }} {{ person.employeeNo }}</span>
        <span v-if="person.department">{{ person.department }}</span>
        <span v-if="person.phone">{{ person.phone }}</span>
      </div>
    </div>

    <template v-if="person">
      <!-- 人脸 -->
      <van-cell-group inset :title="$t('person.face')">
        <div v-for="(f, i) in cred.faces" :key="i" class="face-item">
          <img :src="f.faceBase64" class="face-thumb" @click="previewFace(f)" />
          <div class="face-info">
            <div class="face-name">{{ f.name || $t('person.face') }}</div>
            <van-tag :type="faceStatus(f).type" round size="medium">{{
              faceStatus(f).text
            }}</van-tag>
            <div v-if="faceStatus(f).msg" class="face-msg">{{ faceStatus(f).msg }}</div>
          </div>
          <van-icon name="cross" class="face-del" @click="removeItem('faces', i)" />
        </div>
        <van-empty v-if="!cred.faces.length" :description="$t('person.noFace')" image-size="64" />
        <van-cell :title="$t('person.addFace')" is-link icon="plus" @click="chooseFace" />
      </van-cell-group>

      <!-- IC 卡 -->
      <van-cell-group inset :title="$t('person.icCard')">
        <van-cell
          v-for="(c, i) in cred.icCards"
          :key="i"
          :title="c.name || $t('person.accessCard')"
          :label="c.cardId"
        >
          <template #right-icon
            ><van-icon name="cross" @click="removeItem('icCards', i)"
          /></template>
        </van-cell>
        <van-cell :title="$t('person.addIcCard')" is-link icon="plus" @click="openCard" />
      </van-cell-group>

      <!-- 二维码 -->
      <van-cell-group inset :title="$t('person.qrCode')">
        <div v-for="(q, i) in cred.qrCodes" :key="i" class="qr-item">
          <img
            v-if="qrImageMap[q.qrUnique]"
            :src="qrImageMap[q.qrUnique]"
            class="qr-thumb"
            @click="previewQr(q)"
          />
          <van-icon v-else name="qr" class="qr-thumb-placeholder" />
          <div class="qr-info">
            <div class="qr-name">{{ q.name || $t('person.passCode') }}</div>
            <div class="qr-unique">{{ q.qrUnique }}</div>
          </div>
          <van-icon name="cross" class="qr-del" @click="removeItem('qrCodes', i)" />
        </div>
        <van-empty v-if="!cred.qrCodes.length" :description="$t('person.noQr')" image-size="64" />
        <van-cell :title="$t('person.createQr')" is-link icon="plus" @click="openQr" />
      </van-cell-group>

      <!-- 楼层权限（多选勾选） -->
      <van-cell-group inset :title="$t('person.floorPerm', { n: cred.floors.length })">
        <div class="floor-tags">
          <van-tag v-for="fl in cred.floors" :key="fl" round type="primary" class="floor-tag">
            {{ floorLabel(fl) }}
          </van-tag>
          <span v-if="!cred.floors.length" class="floor-empty">{{ $t('person.noFloor') }}</span>
        </div>
        <van-cell :title="$t('person.setFloors')" is-link icon="plus" @click="openFloorPicker" />
      </van-cell-group>

      <!-- 通行规则（功能开关 + 详情） -->
      <van-cell-group inset :title="$t('person.passRule')">
        <div class="rule-funcs">
          <van-cell v-for="f in functionOptions" :key="f.key" :title="$t(f.labelKey)" center>
            <template #right-icon>
              <van-switch v-model="rules[f.key]" :disabled="f.key === 'relay'" size="20" />
            </template>
          </van-cell>
        </div>

        <!-- 定时规则详情 -->
        <div v-if="rules.time" class="rule-detail">
          <div class="rule-detail-title">{{ $t('person.timingRule') }}</div>
          <div v-for="(r, idx) in rules.timingRules" :key="idx" class="timing-item">
            <span>{{ timingText(r) }}</span>
            <van-icon name="cross" @click="rules.timingRules.splice(idx, 1)" />
          </div>
          <van-button size="small" plain type="primary" round @click="openTiming"
            >+ {{ $t('person.addTiming') }}</van-button
          >
        </div>

        <!-- 次数详情 -->
        <div v-if="rules.count" class="rule-detail">
          <van-field
            v-model.number="rules.countDataAddCount"
            type="number"
            :label="$t('person.perAdd')"
            placeholder="0"
          />
          <van-field
            :model-value="String(rules.countDataTotal)"
            :label="$t('person.totalCount')"
            readonly
          />
        </div>

        <!-- 金额详情 -->
        <div v-if="rules.amount" class="rule-detail">
          <van-field
            v-model.number="rules.amountDataAddAmount"
            type="number"
            :label="$t('person.perAdd')"
            placeholder="0.00"
          />
          <van-field
            :model-value="String(rules.amountDataTotal)"
            :label="$t('person.totalAmount')"
            readonly
          />
          <van-field
            v-model.number="rules.amountDataUnitPrice"
            type="number"
            :label="$t('person.unitPrice')"
            placeholder="0.50"
          />
        </div>

        <!-- 期限详情 -->
        <div v-if="rules.duration" class="rule-detail">
          <van-field
            v-model="rules.durationDataStartTime"
            :label="$t('person.startTime')"
            placeholder="YYYY-MM-DD HH:mm"
          />
          <van-field
            v-model="rules.durationDataEndTime"
            :label="$t('person.endTime')"
            placeholder="YYYY-MM-DD HH:mm"
          />
        </div>

        <!-- 继电器详情 -->
        <div v-if="rules.relay" class="rule-detail">
          <van-checkbox-group v-model="rules.relayStates" direction="horizontal">
            <van-checkbox name="channel1" shape="square">{{ $t('person.channel1') }}</van-checkbox>
            <van-checkbox name="channel2" shape="square">{{ $t('person.channel2') }}</van-checkbox>
          </van-checkbox-group>
          <van-field
            v-model.number="rules.relayOnTime"
            type="number"
            :label="$t('person.relayOnTime')"
            placeholder="0"
          />
        </div>

        <!-- 控梯详情 -->
        <div v-if="rules.controlElevator" class="rule-detail">
          <van-radio-group v-model="rules.controlElevatorDataValueType" direction="horizontal">
            <van-radio :name="1">{{ $t('person.directLift') }}</van-radio>
            <van-radio :name="2">{{ $t('person.accessControl') }}</van-radio>
            <van-radio :name="3">{{ $t('person.repairBox') }}</van-radio>
          </van-radio-group>
        </div>

        <!-- 远程呼梯详情 -->
        <div v-if="rules.remoteCall" class="rule-detail">
          <van-field
            v-model.number="rules.remoteCallDataCurrentDayCallCount"
            type="number"
            :label="$t('person.dailyCallCount')"
          />
          <van-field
            v-model.number="rules.remoteCallDataEveryDayUpdateCallCount"
            type="number"
            :label="$t('person.dailyUpdateCount')"
          />
        </div>

        <!-- 密码通行详情 -->
        <div v-if="rules.cardPassword" class="rule-detail">
          <van-field
            v-model="rules.cardPasswordDataPassword"
            type="password"
            :label="$t('person.passPassword')"
            :placeholder="$t('person.enterPassword')"
          />
        </div>
      </van-cell-group>

      <div class="footer">
        <van-button type="primary" block round :loading="saving" @click="save">{{
          $t('person.saveCredential')
        }}</van-button>
      </div>
    </template>

    <!-- 编辑人员信息弹窗 -->
    <van-dialog
      v-model:show="showEditPerson"
      :title="$t('person.editPerson')"
      :show-confirm-button="false"
    >
      <div class="dialog-form">
        <van-field
          v-model="personForm.name"
          :label="$t('person.name')"
          :placeholder="$t('common.required')"
        />
        <van-field
          v-model="personForm.employeeNo"
          :label="$t('person.employeeNo')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="personForm.department"
          :label="$t('person.department')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="personForm.phone"
          :label="$t('person.phone')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="personForm.account"
          :label="$t('person.account')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="personForm.remark"
          :label="$t('person.remark')"
          :placeholder="$t('common.optional')"
        />
      </div>
      <div class="dialog-actions">
        <van-button block round type="primary" :loading="savingPerson" @click="savePerson">{{
          $t('common.save')
        }}</van-button>
      </div>
    </van-dialog>

    <!-- 录 IC 卡弹窗 -->
    <van-dialog
      v-model:show="showCard"
      :title="$t('person.addIcCard')"
      :show-confirm-button="false"
    >
      <div class="dialog-form">
        <van-field
          v-model="cardForm.name"
          :label="$t('common.name')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="cardForm.cardId"
          :label="$t('person.cardNo')"
          :placeholder="$t('person.cardNoPlaceholder')"
        />
      </div>
      <div class="dialog-actions">
        <van-button block round type="primary" @click="addCard">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-dialog>

    <!-- 创建二维码弹窗 -->
    <van-dialog v-model:show="showQr" :title="$t('person.createQr')" :show-confirm-button="false">
      <div class="dialog-form">
        <van-field
          v-model="qrForm.name"
          :label="$t('common.name')"
          :placeholder="$t('common.optional')"
        />
        <van-field :label="$t('person.type')">
          <template #input>
            <van-radio-group v-model="qrForm.qrType" direction="horizontal">
              <van-radio name="QR_STANDALONE">{{ $t('person.standalone') }}</van-radio>
              <van-radio name="QR_VISITOR">{{ $t('person.visitor') }}</van-radio>
            </van-radio-group>
          </template>
        </van-field>
      </div>
      <div class="dialog-actions">
        <van-field
          v-if="qrForm.qrType === 'QR_VISITOR'"
          v-model="qrForm.validTo"
          label="有效截止时间"
          type="datetime-local"
        />
        <van-button block round type="primary" @click="addQr">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-dialog>

    <!-- 楼层多选弹窗 -->
    <van-popup v-model:show="showFloor" position="bottom" round>
      <div class="floor-pop">
        <div class="pop-title">{{ $t('person.selectFloorsTitle') }}</div>
        <div class="floor-pop-bar">
          <van-checkbox
            :model-value="floorSelected.length === floorOptions.length"
            :indeterminate="floorSelected.length > 0 && floorSelected.length < floorOptions.length"
            @update:model-value="toggleAll"
            >{{ $t('person.selectAll') }}</van-checkbox
          >
          <span class="floor-count">{{
            $t('person.selectedCount', { n: floorSelected.length })
          }}</span>
        </div>
        <van-checkbox-group v-model="floorSelected" class="floor-grid">
          <van-checkbox
            v-for="f in floorOptions"
            :key="f"
            :name="f"
            shape="square"
            class="floor-check"
            >{{ floorLabel(f) }}</van-checkbox
          >
        </van-checkbox-group>
        <van-button type="primary" block round @click="confirmFloor">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-popup>

    <!-- 定时规则弹窗 -->
    <van-popup v-model:show="showTiming" position="bottom" round>
      <div class="timing-pop">
        <div class="pop-title">{{ $t('person.addTiming') }}</div>
        <div class="timing-days">
          <van-checkbox-group v-model="timingForm.days" direction="horizontal">
            <van-checkbox v-for="d in weekOptions" :key="d.value" :name="d.value" shape="square">{{
              $t(d.labelKey)
            }}</van-checkbox>
          </van-checkbox-group>
        </div>
        <van-field v-model="timingForm.start" :label="$t('common.start')" placeholder="HH:mm" />
        <van-field v-model="timingForm.end" :label="$t('common.end')" placeholder="HH:mm" />
        <van-button type="primary" block round @click="confirmTiming">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-popup>

    <input
      ref="fileInput"
      type="file"
      accept="image/*"
      style="display: none"
      @change="onFaceFile"
    />
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog, showImagePreview } from 'vant'
import { api } from '../../api'
import { compressImage } from '../../utils/imageCompress'
import { generateQrDataUrl } from '../../utils/qr'

const route = useRoute()
const router = useRouter()
const { t } = useI18n()

const id = Number(route.params.id)
const person = ref(null)
const cred = reactive({ icCards: [], faces: [], qrCodes: [], rules: {}, floors: [] })
const rules = reactive({})
const saving = ref(false)
const busy = ref(false)
const personVersion = ref(0)

// 默认规则（8 个功能开关 + 详情）

/** @brief 创建规则表单的初始结构，随后由设备实际规则覆盖。 */
const defaultRules = () => ({
  time: false,
  timingRules: [],
  count: false,
  countDataTotal: 0,
  countDataAddCount: 0,
  amount: false,
  amountDataTotal: 0,
  amountDataAddAmount: 0,
  amountDataUnitPrice: 0.5,
  duration: false,
  durationDataStartTime: '',
  durationDataEndTime: '',
  relay: false,
  relayStates: [],
  relayOnTime: 0,
  controlElevator: false,
  controlElevatorDataValueType: 1,
  remoteCall: false,
  remoteCallDataCurrentDayCallCount: 10,
  remoteCallDataEveryDayUpdateCallCount: 10,
  cardPassword: false,
  cardPasswordDataPassword: '',
})

// 通行功能项：label 存 i18n 键，随语言切换重算
const functionOptions = computed(() => [
  { key: 'time', labelKey: 'person.fTime' },
  { key: 'count', labelKey: 'person.fCount' },
  { key: 'amount', labelKey: 'person.fAmount' },
  { key: 'duration', labelKey: 'person.fDuration' },
  { key: 'relay', labelKey: 'person.fRelay' },
  { key: 'controlElevator', labelKey: 'person.fControl' },
  { key: 'remoteCall', labelKey: 'person.fRemoteCall' },
  { key: 'cardPassword', labelKey: 'person.fPassword' },
])

// 星期选项：存 i18n 键
const weekOptions = computed(() => [
  { value: 1, labelKey: 'common.weekShort.mon' },
  { value: 2, labelKey: 'common.weekShort.tue' },
  { value: 3, labelKey: 'common.weekShort.wed' },
  { value: 4, labelKey: 'common.weekShort.thu' },
  { value: 5, labelKey: 'common.weekShort.fri' },
  { value: 6, labelKey: 'common.weekShort.sat' },
  { value: 7, labelKey: 'common.weekShort.sun' },
])

const showCard = ref(false)
const cardForm = reactive({ name: '', cardId: '' })
const showQr = ref(false)
const qrForm = reactive({ name: '', qrType: 'QR_STANDALONE', validTo: '' })
const showFloor = ref(false)
const floorSelected = ref([])
const showTiming = ref(false)
const timingForm = reactive({ days: [1], start: '09:00', end: '18:00' })
const fileInput = ref(null)
const faceResults = ref([])
const qrImageMap = ref({})
const showEditPerson = ref(false)
const savingPerson = ref(false)
const personForm = reactive({
  name: '',
  employeeNo: '',
  department: '',
  phone: '',
  account: '',
  remark: '',
})

// 楼层选项：-8~-1 地下、1~120 地上（无 0）
const floorOptions = [
  ...Array.from({ length: 8 }, (_, i) => -(i + 1)).sort((a, b) => a - b),
  ...Array.from({ length: 120 }, (_, i) => i + 1),
]

/** 楼层显示：负楼层 B，正楼层 n 楼（参数插值，随语言切换） */
function floorLabel(f) {
  if (f < 0) return t('common.floorBasement', { n: Math.abs(f) })
  return t('common.floorN', { n: f })
}

/** 定时规则文案：周(几) 时间段 */
function timingText(r) {
  const shortKeys = [
    'common.weekShort.mon',
    'common.weekShort.tue',
    'common.weekShort.wed',
    'common.weekShort.thu',
    'common.weekShort.fri',
    'common.weekShort.sat',
    'common.weekShort.sun',
  ]
  const days =
    (r.days || []).map((d) => t(shortKeys[d - 1])).join(t('common.scheduleJoin')) ||
    t('person.notSelected')
  const range = (r.timeRange || ['--:--', '--:--']).join(' ~ ')
  const weeks = t('common.weekParen', { days })
  return t('common.timingLine', { weeks, range })
}

/** @brief 并行读取本地人员与凭证，刷新规则、人脸结果和二维码预览。 */
async function load() {
  try {
    const { person: p, credential: c } = await api.personDetail({ id })
    person.value = p
    personVersion.value = c.version
    cred.icCards = c.icCards || []
    cred.faces = c.faces || []
    cred.qrCodes = c.qrCodes || []
    cred.floors = c.floors || []
    Object.assign(rules, defaultRules(), c.rules || {})
    cred.rules = rules
    await loadFaceRegister()
    loadQrImages()
  } catch (error) {
    showToast(error.message || t('common.loadFailed'))
  }
}

/** @brief 响应用户操作，打开本地人脸图片选择器。 */
function chooseFace() {
  fileInput.value && fileInput.value.click()
}

/** @brief 压缩单张图片并提交本地人脸任务，完成后刷新凭证。 */
async function onFaceFile(e) {
  const file = e.target.files?.[0]
  e.target.value = ''
  if (!file || busy.value) return
  busy.value = true
  try {
    const faceBase64 = await compressImage(file, 250, 1280)
    await api.addSubItem({
      personId: id,
      subType: 'FACE',
      version: personVersion.value,
      subData: { faceBase64, name: t('person.faceName', { name: person.value.name }) },
    })
    await load()
    showToast(t('common.saved'))
  } catch (error) {
    showToast(error.message)
    await loadFaceRegister()
  } finally {
    busy.value = false
  }
}

// 人脸注册结果：按 faceBase64 匹配（真实场景由 QT 将 Imagesresult 映射为每张人脸结果）

/** @brief 按本地 faceId 优先匹配注册结果，未完成项保持等待状态。 */
function faceStatus(f) {
  const r = faceResults.value.find(
    (x) => (f.faceId && x.faceId === f.faceId) || x.faceBase64 === f.faceBase64,
  )
  if (!r || r.result == null)
    return { type: 'default', text: t('person.faceUnreported'), msg: r?.msg || '' }
  return {
    type: r.result === 1 ? 'success' : 'danger',
    text: t(r.result === 1 ? 'person.faceRegistered' : 'person.faceFailed'),
    msg: r.msg || '',
  }
}

/** @brief 预览当前凭证中的实际人脸图片。 */
function previewFace(f) {
  if (f.faceBase64) showImagePreview([f.faceBase64])
}

/** @brief 读取本地人脸任务结果，查询失败不覆盖人员资料。 */
async function loadFaceRegister() {
  try {
    const r = await api.faceRegisterResult({ id })
    faceResults.value = (r && r.results) || []
  } catch (e) {
    // 注册结果加载失败不影响详情展示
    faceResults.value = []
  }
}

/** @brief 确认后按子项类型与人员版本撤销指定凭证。 */
async function removeItem(key, index) {
  if (busy.value) return
  const item = cred[key][index]
  const subType = key === 'faces' ? 'FACE' : key === 'icCards' ? 'IC_CARD' : item.qrType
  const itemId = item.faceId || item.cardId || item.qrUnique
  try {
    await showConfirmDialog({
      title: t('common.delete'),
      message: t('common.deleteConfirm', { name: item.name || itemId }),
    })
    busy.value = true
    await api.removeSubItem({ personId: id, subType, itemId, version: personVersion.value })
    await load()
  } catch (error) {
    if (error?.message) showToast(error.message)
  } finally {
    busy.value = false
  }
}

/** @brief 清空录卡表单并打开对话框。 */
function openCard() {
  cardForm.name = ''
  cardForm.cardId = ''
  showCard.value = true
}

/** @brief 提交单张门禁卡并刷新本地凭证列表。 */
async function addCard() {
  if (!cardForm.cardId || busy.value) {
    showToast(t('person.enterCardNo'))
    return
  }
  busy.value = true
  try {
    await api.addSubItem({
      personId: id,
      subType: 'IC_CARD',
      version: personVersion.value,
      subData: { cardId: cardForm.cardId, name: cardForm.name || t('person.accessCard') },
    })
    showCard.value = false
    await load()
  } catch (error) {
    showToast(error.message)
  } finally {
    busy.value = false
  }
}

/** @brief 初始化二维码类型和名称草稿。 */
function openQr() {
  qrForm.name = ''
  qrForm.qrType = 'QR_STANDALONE'
  showQr.value = true
}

/** @brief 生成二维码编号并提交所选类型及访客截止时间。 */
async function addQr() {
  if (busy.value) return
  const bytes = new Uint32Array(1)
  crypto.getRandomValues(bytes)
  const qrUnique = String(10000000 + (bytes[0] % 90000000))
  busy.value = true
  try {
    await api.addSubItem({
      personId: id,
      subType: qrForm.qrType,
      version: personVersion.value,
      subData: {
        qrUnique,
        qrType: qrForm.qrType,
        name: qrForm.name || t('person.passCode'),
        ...(qrForm.qrType === 'QR_VISITOR'
          ? { validTo: new Date(qrForm.validTo).toISOString() }
          : {}),
      },
    })
    showQr.value = false
    await load()
  } catch (error) {
    showToast(error.message)
  } finally {
    busy.value = false
  }
}

// 按 qrUnique 生成二维码缩略图

/** @brief 按设备返回的二维码编号生成本地预览图。 */
async function genQr(q) {
  if (!q.qrUnique) return
  try {
    qrImageMap.value[q.qrUnique] = await generateQrDataUrl(q.qrUnique)
  } catch (e) {
    // 生成失败保持占位图标
  }
}

/** @brief 为已保存二维码补齐页面预览图。 */
function loadQrImages() {
  cred.qrCodes.forEach((q) => genQr(q))
}

/** @brief 打开当前二维码的图片预览。 */
function previewQr(q) {
  const src = qrImageMap.value[q.qrUnique]
  if (src) showImagePreview([src])
}

/** @brief 从实际楼层集合创建独立的选择草稿。 */
function openFloorPicker() {
  floorSelected.value = [...(cred.floors || [])]
  showFloor.value = true
}

/** @brief 在楼层选择草稿中全选或清空。 */
function toggleAll(checked) {
  floorSelected.value = checked ? [...floorOptions] : []
}

/** @brief 将已选楼层排序后写回待提交表单。 */
function confirmFloor() {
  cred.floors = [...floorSelected.value].sort((a, b) => a - b)
  showFloor.value = false
}

/** @brief 初始化通行时段编辑表单。 */
function openTiming() {
  timingForm.days = [1]
  timingForm.start = '09:00'
  timingForm.end = '18:00'
  showTiming.value = true
}

/** @brief 校验星期选择后把通行时段加入规则草稿。 */
function confirmTiming() {
  if (!timingForm.days.length) {
    showToast(t('person.selectWeekday'))
    return
  }
  rules.timingRules.push({
    days: [...timingForm.days],
    timeRange: [timingForm.start, timingForm.end],
  })
  showTiming.value = false
}

/** @brief 依次保存规则与楼层，区分全部完成和部分保存失败。 */
async function save() {
  if (saving.value || busy.value) return
  saving.value = true
  let rulesSaved = false
  try {
    const c = await api.credentialUpdate({
      personId: id,
      version: personVersion.value,
      extra: { rules: JSON.parse(JSON.stringify(rules)) },
    })
    rulesSaved = true
    personVersion.value = c.version
    await api.personFloorSave({ personId: id, version: c.version, floor: [...cred.floors] })
    showToast(t('common.saved'))
    await load()
  } catch (error) {
    showToast((rulesSaved ? '规则已保存，楼层保存失败：' : '') + error.message)
  } finally {
    saving.value = false
  }
}

/** @brief 确认后按当前版本软删除人员并退出详情页。 */
async function onDeletePerson() {
  try {
    await showConfirmDialog({
      title: t('person.deletePerson'),
      message: t('person.deletePersonWarn', { name: person.value.name }),
    })
    await api.personDelete({ id, version: personVersion.value })
    showToast({ type: 'success', message: t('common.deleted') })
    router.back()
  } catch (error) {
    if (error?.message) showToast(error.message)
  }
}

/** @brief 从人员实际资料填充编辑表单。 */
function openEditPerson() {
  personForm.name = person.value.name || ''
  personForm.employeeNo = person.value.employeeNo || ''
  personForm.department = person.value.department || ''
  personForm.phone = person.value.phone || ''
  personForm.account = person.value.account || ''
  personForm.remark = person.value.remark || ''
  showEditPerson.value = true
}

/** @brief 按当前版本提交基本资料，成功后重新读取设备状态。 */
async function savePerson() {
  if (!personForm.name || savingPerson.value) return
  savingPerson.value = true
  try {
    await api.personUpdate({ id, ...personForm, version: personVersion.value })
    showEditPerson.value = false
    await load()
    showToast(t('common.saved'))
  } catch (error) {
    showToast(error.message)
  } finally {
    savingPerson.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.local-busy {
  margin: 40vh auto 0;
  width: max-content;
  color: white;
  background: #333;
  padding: 20px;
  border-radius: 12px;
}
.page {
  padding-bottom: 80px;
}
.person-card {
  padding: 20px var(--page-pad);
  background: var(--card);
  margin: var(--gap) var(--page-pad);
  border-radius: var(--radius-card);
  box-shadow: var(--shadow-card);
}
.person-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}
.person-name {
  font-size: 22px;
  font-weight: 600;
}
.person-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  color: var(--text-3);
  font-size: 13px;
}
.dialog-form {
  padding: 0 var(--page-pad);
}
.dialog-actions {
  padding: var(--page-pad);
}

.face-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 16px;
}
.face-thumb {
  width: 48px;
  height: 48px;
  border-radius: 8px;
  object-fit: cover;
  background: #eef3fb;
  flex-shrink: 0;
}
.face-info {
  flex: 1;
  min-width: 0;
}
.face-name {
  font-size: 15px;
  margin-bottom: 4px;
}
.face-msg {
  margin-top: 4px;
  font-size: 12px;
  color: var(--van-danger-color, #ee0a24);
}
.face-del {
  color: var(--van-gray-5, #c8c9cc);
  font-size: 18px;
  padding: 8px;
}

.qr-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 16px;
}
.qr-thumb {
  width: 48px;
  height: 48px;
  border-radius: 6px;
  background: #fff;
  padding: 3px;
  box-sizing: border-box;
  flex-shrink: 0;
}
.qr-thumb-placeholder {
  width: 48px;
  height: 48px;
  border-radius: 6px;
  background: #f5f6f8;
  color: var(--text-3);
  font-size: 30px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.qr-info {
  flex: 1;
  min-width: 0;
}
.qr-name {
  font-size: 15px;
  margin-bottom: 2px;
}
.qr-unique {
  font-size: 12px;
  color: var(--text-3);
}
.qr-del {
  color: var(--van-gray-5, #c8c9cc);
  font-size: 18px;
  padding: 8px;
}

.floor-tags {
  padding: 8px 16px;
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.floor-tag {
  background: #e6f7ef;
  color: var(--primary);
}
.floor-empty {
  color: var(--text-3);
  font-size: 13px;
}

.rule-funcs {
  border-top: 1px solid #f2f3f5;
}
.rule-detail {
  padding: 8px 16px;
  background: #fafbfc;
  border-top: 1px solid #f2f3f5;
}
.rule-detail-title {
  font-size: 13px;
  color: var(--text-2);
  margin: 6px 0;
}
.timing-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 6px 0;
  font-size: 13px;
  color: var(--text-2);
}
.timing-pop,
.floor-pop {
  padding: var(--page-pad);
}
.pop-title {
  font-size: 18px;
  font-weight: 600;
  text-align: center;
  margin-bottom: 12px;
}
.timing-days {
  margin-bottom: 8px;
}
.floor-pop-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 4px 0 8px;
}
.floor-count {
  color: var(--text-3);
  font-size: 13px;
}
.floor-grid {
  max-height: 45vh;
  overflow-y: auto;
  display: flex;
  flex-wrap: wrap;
  margin-bottom: 12px;
}
.floor-check {
  width: 33.33%;
  margin: 4px 0;
}
.footer {
  padding: var(--page-pad);
}
</style>
