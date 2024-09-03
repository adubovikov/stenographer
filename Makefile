NAME?=stenographer

all:
		go build -a -o $(NAME)
        #go build -a -ldflags '-extldflags "-static"' -o $(NAME) 

debug:
		go build -o $(NAME) 

docker:
        CGO_ENABLED=1 GOOS=linux go build -a --ldflags '-linkmode external -extldflags "-static -s -w"' -o $(NAME)

modules:
		go get ./...

.PHONY: clean
clean:
		rm -fr $(NAME)
