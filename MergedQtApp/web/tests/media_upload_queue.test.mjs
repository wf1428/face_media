/**
 * @fileoverview 验证媒体选择队列的跨次合并、移除后重选和容量边界，不分配大文件内容。
 * @author Du lin
 * @date 2026-10-22
 */
import test from 'node:test'
import assert from 'node:assert/strict'
import { MEDIA_UPLOAD_LIMITS, mergeMediaSelection } from '../frontend/src/utils/mediaUploadQueue.js'

const MiB = 1024 * 1024

/**
 * 创建只含文件名与长度的选择元数据，边界用例不占用等量内存。
 * @param {number} count 文件个数。
 * @param {string} prefix 名称前缀，用于模拟不同次选择。
 * @returns {object[]} 混合图片、视频文件描述。
 */
function files(count, prefix) {
  return Array.from({ length: count }, (_, n) => ({ name: `${prefix}-${n}.${n % 2 ? 'png' : 'mp4'}`, size: 100 }))
}

test('five selected files plus another ten cannot bypass the cumulative count limit', () => {
  const current = files(5, 'first')
  const result = mergeMediaSelection(current, files(10, 'second'))
  assert.equal(result.error.key, 'video.queueCountLimit')
  assert.equal(result.error.params.remaining, 5)
  assert.deepEqual(result.files, current)
  assert.equal(current.length, 5)
})

test('two selections can fill the ten-item queue; removing an item allows another selection', () => {
  const first = mergeMediaSelection([], files(5, 'first'))
  const full = mergeMediaSelection(first.files, files(5, 'second'))
  assert.equal(full.error, null)
  assert.equal(full.files.length, 10)
  const afterRemoval = full.files.filter((_, index) => index !== 3)
  const replaced = mergeMediaSelection(afterRemoval, files(1, 'replacement'))
  assert.equal(replaced.error, null)
  assert.equal(replaced.files.length, 10)
  assert.equal(replaced.files.at(-1).name, 'replacement-0.mp4')
})

test('reselecting names, including different extension case, does not duplicate entries', () => {
  const original = [{ name: '海报.PNG', size: 20 }]
  const result = mergeMediaSelection(original, [{ name: '海报.png', size: 20 }, ...files(1, 'new')])
  assert.equal(result.error, null)
  assert.equal(result.skipped, 1)
  assert.equal(result.files.length, 2)
  assert.equal(result.files[0], original[0])
})

test('video and image limits are inclusive and oversized selections leave the old queue intact', () => {
  for (const [name, size, error] of [
    ['video.mp4', 500 * MiB, 'video.queueVideoLimit'],
    ['image.webp', 100 * MiB, 'video.queueImageLimit'],
  ]) {
    assert.equal(mergeMediaSelection([], [{ name, size }]).error, null)
    const current = files(2, 'kept')
    const invalid = mergeMediaSelection(current, [{ name, size: size + 1 }, ...files(1, 'not-added')])
    assert.equal(invalid.error.key, error)
    assert.deepEqual(invalid.files, current)
  }
})

test('combined size is capped at five GiB and a maximum valid video queue remains accepted', () => {
  assert.equal(MEDIA_UPLOAD_LIMITS.totalBytes, 5 * 1024 ** 3)
  const full = Array.from({ length: 10 }, (_, n) => ({ name: `${n}.mp4`, size: 500 * MiB }))
  assert.equal(mergeMediaSelection([], full).error, null)
  const overTotal = full.map((file) => ({ ...file, size: 512 * MiB + 1 }))
  assert.equal(mergeMediaSelection([], overTotal).error.key, 'video.queueTotalLimit')
})

test('invalid, empty, and unsupported files are rejected before any selection is submitted', () => {
  for (const size of [0, -1, NaN, Infinity, 0.5])
    assert.equal(mergeMediaSelection([], [{ name: 'bad.png', size }]).error.key, 'video.queueInvalidSize')
  assert.equal(mergeMediaSelection([], [{ name: 'bad.exe', size: 100 }]).error.key, 'video.queueInvalidType')
  assert.equal(mergeMediaSelection([], []).files.length, 0)
})
