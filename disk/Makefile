AGUIDECHECK=Tools/AGuideCheck/AGuideCheck

disk:
	./createdisk.sh

clean:
	rm -f A4091_????.adf
	rm -f NDK3.2.lha
	rm -rf Tools

check: $(AGUIDECHECK)
	vamos $(AGUIDECHECK) A4091.guide

$(AGUIDECHECK):
	wget http://aminet.net/dev/misc/NDK3.2.lha
	lha xqf NDK3.2.lha Tools/AGuideCheck/* Tools/AG2HTML/*
