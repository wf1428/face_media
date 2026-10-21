<!--
 @file UnsupportedPanel.vue
 @brief 保留页面外观，禁用鼠标/键盘操作，不执行未接入业务。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <section class="unsupported">
    <van-notice-bar
      text="本版本未接入；以下控件仅保留界面，状态尚未读取。"
      color="#6b7280"
      background="#eef2f6"
    />
    <div class="unsupported-content" inert aria-disabled="true"><slot /></div>
  </section>
</template>

<style scoped>
.unsupported {
  margin-bottom: 12px;
}
.unsupported-content {
  pointer-events: none;
  opacity: 0.65;
  user-select: none;
}
</style>
