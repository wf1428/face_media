"""
@file make_fixture.py
@brief 使用标准 ZIP/XML 生成原 Qt 模板字段的 Excel 测试输入，不依赖 Excel 软件。
@author Du lin
@date 2026-10-09
"""

from pathlib import Path
from xml.sax.saxutils import escape
import sys
import zipfile


def create(path: Path) -> None:
    """生成有效行、文件内重复账号、无效手机号和示例行混合的 XLSX。"""
    # 列顺序与 Qt 的本地人员 Excel 模板保持一致。
    rows = [
        ["姓名", "账号", "工号", "手机号", "IC卡号", "二维码", "设备及楼层", "启用功能"],
        ["测试甲", "web_a", "W001", "13812345678", "", "", "", ""],
        ["重复甲", "WEB_A", "W002", "13812345679", "", "", "", ""],
        ["无效电话", "bad", "W003", "123", "", "", "", ""],
        ["网络重复", "network_a", "W004", "13812345680", "", "", "", ""],
        ["测试乙", "web_b", "W005", "13812345681", "ABCD1234", "", "", ""],
    ]
    content = []
    for index, row in enumerate(rows, 1):
        cells = "".join(
            f'<c r="{chr(65+i)}{index}" t="inlineStr"><is><t>{escape(value)}</t></is></c>'
            for i, value in enumerate(row)
        )
        content.append(f'<row r="{index}">{cells}</row>')
    worksheet = (
        '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
        "<sheetData>" + "".join(content) + "</sheetData></worksheet>"
    )

    # 直接写入最小 XLSX 结构，测试输入不依赖桌面 Excel 软件。
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(
            "xl/workbook.xml",
            '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
            'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            '<sheets><sheet name="人员列表" sheetId="1" r:id="rId1"/></sheets></workbook>',
        )
        archive.writestr(
            "xl/_rels/workbook.xml.rels",
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Target="worksheets/sheet1.xml" '
            'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"/>'
            "</Relationships>",
        )
        archive.writestr("xl/worksheets/sheet1.xml", worksheet)


if __name__ == "__main__":
    create(Path(sys.argv[1]))
