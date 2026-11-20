all: proxy app agent

proxy:
	mkdir -p build
	cd proxy && go build -o ../build/xpproxy .

app:
	$(MAKE) -C app

agent:
	$(MAKE) -C guest

clean:
	$(MAKE) -C app clean
	$(MAKE) -C guest clean
	rm -rf build

.PHONY: all proxy app agent clean
