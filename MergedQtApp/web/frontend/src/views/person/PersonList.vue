<!--
 @file PersonList.vue
 @brief 本地人员管理与原 Excel/人脸批次导入入口。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('person.title') }}</div>
      <van-button size="small" round type="primary" @click="openAdd">{{
        $t('person.createAccount')
      }}</van-button>
    </div>

    <PersonnelImport @changed="load" />

    <van-cell-group inset title="NFC-读卡" class="nfc-card">
      <button
        type="button"
        class="nfc-image-button"
        :aria-label="t('person.viewNfcImage')"
        @click="showNfc = true"
      >
        <img src="/NFC-readCard.jpg" :alt="t('person.nfcImage')" class="nfc-image" />
      </button>
    </van-cell-group>

    <van-popup v-model:show="showNfc" round closeable class="nfc-popup">
      <img src="/NFC-readCard.jpg" :alt="t('person.nfcImage')" class="nfc-preview" />
      <a class="nfc-save" href="/NFC-readCard.jpg" download="NFC-readCard.jpg">{{
        t('person.saveNfcImage')
      }}</a>
    </van-popup>

    <van-cell-group inset v-if="persons.length">
      <van-cell
        v-for="p in persons"
        :key="p.id"
        :title="p.name"
        :label="personLabel(p)"
        icon="friends-o"
        is-link
        @click="goDetail(p)"
      >
        <template #right-icon>
          <van-icon name="delete-o" class="del-icon" @click.stop="onDelete(p)" />
        </template>
      </van-cell>
    </van-cell-group>

    <van-empty v-else :description="$t('person.noPersons')" />

    <van-dialog
      v-model:show="showAdd"
      :title="$t('person.createAccount')"
      :show-confirm-button="false"
      @closed="resetForm"
    >
      <button
        type="button"
        class="dialog-close"
        :aria-label="t('person.closeCreate')"
        :disabled="saving"
        @click="showAdd = false"
      >
        <van-icon name="cross" />
      </button>
      <div class="form">
        <van-field
          v-model="form.name"
          :label="$t('person.name')"
          :placeholder="$t('common.required')"
        />
        <van-field
          v-model="form.employeeNo"
          :label="$t('person.employeeNo')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="form.department"
          :label="$t('person.department')"
          :placeholder="$t('common.optional')"
        />
        <van-field
          v-model="form.phone"
          :label="$t('person.phone')"
          :placeholder="$t('common.optional')"
        />
      </div>
      <div class="form-actions">
        <van-button block round type="primary" :loading="saving" @click="onAdd">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-dialog>
  </div>
</template>

<script setup>
import { ref, onMounted, reactive } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'
import PersonnelImport from '../../components/PersonnelImport.vue'

const router = useRouter()
const { t } = useI18n()
const persons = ref([])
const showAdd = ref(false)
const showNfc = ref(false)
const saving = ref(false)
const form = reactive({ name: '', employeeNo: '', department: '', phone: '' })

/** @brief 人员行副标题：工号 + 部门（无部门显示“未分配”） */
function personLabel(p) {
  const dept = p.department || t('person.unassignedDept')
  return p.employeeNo
    ? `${t('person.employeeNoLine', { no: p.employeeNo })} · ${dept}`
    : p.department || ''
}

/** @brief 清空保留的新增人员表单字段。 */
function resetForm() {
  form.name = ''
  form.employeeNo = ''
  form.department = ''
  form.phone = ''
}

/** @brief 从设备读取并刷新本地人员列表。 */
async function load() {
  try {
    persons.value = await api.personList()
  } catch (error) {
    showToast(error.message)
  }
}

/** @brief 打开选中人员的详情页面。 */
function goDetail(p) {
  router.push('/person/' + p.id)
}

/** @brief 打开本地人员新建表单。 */
function openAdd() {
  showAdd.value = true
}

/** @brief 通过本地 API 新增人员。 */
async function onAdd() {
  if (!form.name) {
    showToast(t('person.nameRequired'))
    return
  }
  saving.value = true
  try {
    await api.personAdd({ ...form })
    showToast({ type: 'success', message: t('person.created') })
    showAdd.value = false
    load()
  } catch (e) {
    showToast({ type: 'fail', message: e.message || t('person.createFailed') })
  } finally {
    saving.value = false
  }
}

/** @brief 删除所选本地人员。 */
async function onDelete(p) {
  try {
    await showConfirmDialog({
      title: t('person.deletePerson'),
      message: t('common.deleteConfirm', { name: p.name }),
    })
    await api.personDelete({ id: p.id, version: p.version })
    showToast(t('common.deleted'))
    await load()
  } catch (error) {
    if (error?.message) showToast(error.message)
  }
}

onMounted(load)
</script>

<style scoped>
.page {
  padding-top: var(--page-pad);
}
.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 var(--page-pad) var(--gap);
}
.title {
  font-size: 20px;
  font-weight: 600;
}
.del-icon {
  color: var(--van-danger-color, #ee0a24);
  font-size: 20px;
}
.form {
  padding: 0 var(--page-pad);
}
.dialog-close {
  position: absolute;
  top: 16px;
  right: 14px;
  border: 0;
  background: transparent;
  color: var(--text-2);
  padding: 6px;
  font-size: 20px;
}
.nfc-image-button {
  display: block;
  width: 100%;
  border: 0;
  background: transparent;
  padding: 16px;
  cursor: pointer;
}
.nfc-image {
  display: block;
  width: min(100%, 220px);
  height: auto;
  margin: auto;
}
.nfc-popup {
  width: min(90vw, 420px);
  padding: 42px 20px 22px;
}
.nfc-preview {
  display: block;
  width: 100%;
  height: auto;
}
.nfc-save {
  display: block;
  margin-top: 20px;
  padding: 12px;
  border-radius: 20px;
  background: var(--primary);
  color: white;
  text-align: center;
}
.form-actions {
  padding: var(--page-pad);
}
</style>
