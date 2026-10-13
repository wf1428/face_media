<!--
 @file PersonList.vue
 @brief 本地人员只读列表与 Excel/人脸批次导入入口。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('person.title') }}</div>
      <van-button size="small" round type="primary" disabled>{{ $t('person.createAccount') }}</van-button>
    </div>

    <PersonnelImport @changed="load" />
    <van-notice-bar text="仅导入本地人员 Excel。原创建、编辑和删除控件保留，本版本未接入。" />

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
          <van-icon name="delete-o" class="del-icon" aria-disabled="true" />
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
      <div class="form">
        <van-field v-model="form.name" :label="$t('person.name')" :placeholder="$t('common.required')" />
        <van-field v-model="form.employeeNo" :label="$t('person.employeeNo')" :placeholder="$t('common.optional')" />
        <van-field v-model="form.department" :label="$t('person.department')" :placeholder="$t('common.optional')" />
        <van-field v-model="form.phone" :label="$t('person.phone')" :placeholder="$t('common.optional')" />
      </div>
      <div class="form-actions">
        <van-button block round type="primary" :loading="saving" @click="onAdd">{{ $t('common.confirm') }}</van-button>
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
const saving = ref(false)
const form = reactive({ name: '', employeeNo: '', department: '', phone: '' })

/** 人员行副标题：工号 + 部门（无部门显示“未分配”） */
function personLabel(p) {
  const dept = p.department || t('person.unassignedDept')
  return p.employeeNo ? `${t('person.employeeNoLine', { no: p.employeeNo })} · ${dept}` : (p.department || '')
}

function resetForm() {
  form.name = ''
  form.employeeNo = ''
  form.department = ''
  form.phone = ''
}

async function load() {
  try { persons.value = await api.personList() } catch (error) { showToast(error.message) }
}

function goDetail(p) {
  router.push('/person/' + p.id)
}

function openAdd() { /* 原控件保留，本版本不执行该业务。 */ }

async function onAdd() { /* 原控件保留，本版本不执行该业务。 */ }

async function onDelete(p) { /* 原控件保留，本版本不执行该业务。 */ }

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
.form-actions {
  padding: var(--page-pad);
}
</style>
