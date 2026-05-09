---
name: release
description: '打包发布 tool_wave 到 GitHub。USE FOR: 发布版本、打 tag、创建 GitHub Release、构建打包。'
---

# tool_wave Release Workflow

## 适用场景

- 发布新版本、创建 Git Tag 和 GitHub Release

## 核心流程

```
1. 构建 → 2. Git 提交推送 → 3. 创建 Tag → 4. GitHub Release
```

## 1. 构建

```bash
make dist
```

输出 `dist/` 目录。

## 2. Git 提交推送

```bash
git add -A
git commit -m "v{VERSION}: {描述}"
git push origin main
```

## 3. 创建 Tag

```bash
git tag -a v{VERSION} -m "v{VERSION}: {描述}"
git push origin v{VERSION}
```

## 4. GitHub Release

```bash
gh release create v{VERSION} \
  --title "v{VERSION} - {标题}" \
  --notes "## 更新内容
- 变更1"
```

> 注意: 远程仓库名为 `Johnmc104/tool_debug`

## 常用操作

```bash
# 删除已有 tag/release 重新发布
gh release delete v{VERSION} --yes
git tag -d v{VERSION} && git push origin :refs/tags/v{VERSION}
```
