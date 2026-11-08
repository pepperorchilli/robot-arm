# 本地跑测试的快捷入口。
# 不往 server/package.json 里塞脚本，是为了让测试层和被测代码完全解耦：
# 删掉 tests/ 目录，服务端照常运行、照常 npm start。
VENV ?= .venv
PY   := $(VENV)/bin/python
PIP  := $(VENV)/bin/pip

.PHONY: setup test fast perf lint fmt report clean

setup:                 ## 建虚拟环境并装依赖
	python3 -m venv $(VENV)
	$(PIP) install -U pip
	$(PIP) install -r requirements.txt

test:                  ## 全量
	$(PY) -m pytest

fast:                  ## 跳过性能基线，改代码时用这个
	$(PY) -m pytest -m "not perf"

perf:                  ## 只跑时延统计，并打印分位数
	$(PY) -m pytest -m perf -s

lint:
	$(VENV)/bin/ruff check .

fmt:                   ## 自动修复可以安全修复的问题
	$(VENV)/bin/ruff check . --fix

report:                ## 生成 junit + 单文件 HTML 报告
	mkdir -p reports
	$(PY) -m pytest --junitxml=reports/junit.xml \
		--html=reports/report.html --self-contained-html

clean:
	rm -rf .pytest_cache .ruff_cache reports
	find . -name __pycache__ -type d -exec rm -rf {} +
