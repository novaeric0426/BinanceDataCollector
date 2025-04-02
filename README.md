# Binance 데이터 수집기

바이낸스에서 실시간 암호화폐 시장 데이터를 수집, 저장 및 모니터링하는 고성능 시스템입니다.
![image](https://github.com/user-attachments/assets/b1464ace-ff80-44d5-8968-4cf723d982a9)


## 개요

이 프로젝트는 바이낸스 WebSocket API에서 실시간 시장 데이터를 캡처하고 분석 및 모니터링 가능하게 만드는 솔루션을 제공합니다. 시스템은 두 가지 주요 구성 요소로 이루어져 있습니다:

1. **데이터 수집기(Data Collector)**: 바이낸스 WebSocket API에 연결하여 거래 및 캔들스틱 데이터를 캡처하고, 장기 저장을 위해 파일에 저장하며 실시간 접근을 위해 공유 메모리에도 저장합니다.

2. **공유 메모리 리더(Shared Memory Reader)**: 데이터 수집 프로세스에 영향을 주지 않고 공유 메모리에서 최신 시장 데이터를 읽고 표시하는 모니터링 도구입니다.

## 기능

- 바이낸스 선물 시장에서 거래 및 캔들스틱 데이터 실시간 수집
- 여러 암호화폐 거래 쌍 동시 지원
- 프로세스 간 통신을 위한 효율적인 공유 메모리 구현
- 이진 형식으로 시장 데이터 영구 저장
- 사용자 정의 가능한 디스플레이 옵션을 갖춘 실시간 모니터링 기능
- 스레드 안전 설계

## 요구 사항

- Linux 기반 운영 체제
- GCC 컴파일러
- libwebsockets (WebSocket 통신용)
- json-c (JSON 파싱용)
- POSIX 공유 메모리 및 스레딩 지원

## 설치

### 의존성 설치

```bash
# Debian 기반 시스템(Ubuntu 등)의 경우
sudo apt-get update
sudo apt-get install build-essential libwebsockets-dev libjson-c-dev

# Red Hat 기반 시스템(Fedora, CentOS 등)의 경우
sudo dnf install gcc make libwebsockets-devel json-c-devel
```

### 클론 및 컴파일

```bash
# 저장소 클론(git 사용 시)
git clone https://github.com/novaeric0426/BinanceDataCollector.git

# 애플리케이션 컴파일
gcc -o binance_data_collector binance_data_collector.c -lrt -lpthread -lwebsockets -ljson-c
gcc -o binance_shared_memory_reader binance_shared_memory_reader.c -lrt -lpthread
```

## 사용 방법

### 데이터 수집기

데이터 수집기는 바이낸스에 연결하여 시장 데이터 캡처를 시작합니다:

```bash
./binance_data_collector -s btcusdt,ethusdt -o /path/to/output/directory
```

옵션:
- `-s, --symbol`: 쉼표로 구분된 거래 쌍 목록(예: btcusdt,ethusdt)
- `-o, --output`: 데이터 파일을 저장할 출력 디렉토리(기본값: ./data)
- `-h, --help`: 도움말 정보 표시

### 공유 메모리 리더

리더는 공유 메모리에 저장된 최신 시장 데이터를 표시합니다:

```bash
./binance_shared_memory_reader -s BTCUSDT -c -i 500
```

옵션:
- `-s SYMBOL`: 특정 심볼의 데이터 표시(예: BTCUSDT)
- `-c`: 연속 모드 - 디스플레이를 주기적으로 업데이트
- `-i INTERVAL`: 연속 모드의 업데이트 간격(밀리초)(기본값: 1000)
- `-n COUNT`: 심볼당 표시할 최대 레코드 수(기본값: 10)
- `-h`: 도움말 정보 표시

## 시스템 아키텍처

### 구성 요소

1. **binance_common.h**: 수집기와 리더 간에 공유되는 공통 정의 및 구조체
2. **binance_data_collector.c**: 주요 데이터 수집 프로그램
3. **binance_shared_memory_reader.c**: 데이터 모니터링 프로그램

### 데이터 흐름

1. 수집기는 바이낸스 WebSocket API에 연결하고 거래 및 kline(캔들스틱) 스트림을 구독합니다
2. 수신된 데이터는 처리되어 메모리 버퍼에 저장되고 이진 파일에 기록됩니다
3. 가장 최근의 데이터는 실시간 접근을 위해 공유 메모리에 유지됩니다
4. 리더는 이 공유 메모리에 접근하여 수집 프로세스에 영향을 주지 않고 최신 시장 데이터를 표시할 수 있습니다

### 공유 메모리 구조

공유 메모리는 다음과 같이 구성됩니다:
- 메타데이터와 심볼 정보가 포함된 헤더 섹션
- 각 거래 쌍에 대해 동일한 크기의 버퍼로 나뉜 데이터 섹션
- 각 심볼의 버퍼는 헤더와 함께 가장 최근의 거래 및 캔들스틱 레코드를 저장합니다

## 데이터 유형

시스템은 두 가지 주요 유형의 시장 데이터를 캡처합니다:

1. **거래 데이터(Trade Data)**: 개별 거래 실행 정보, 포함 내용:
   - 거래 가격 및 수량
   - 거래 시간
   - 거래 ID
   - 구매자/메이커 정보

2. **캔들스틱 데이터(Candlestick Data)**: 고정 시간 간격에 대한 집계된 가격 정보, 포함 내용:
   - 시가, 고가, 저가 및 종가
   - 거래량
   - 거래 횟수
   - 간격 시작 및 종료 시간

## 성능 고려사항

- 데이터 수집기는 성능 영향을 최소화하기 위해 공유 메모리 업데이트를 위한 전용 스레드를 사용합니다
- 시스템은 여러 거래 쌍에서 동시에 높은 메시지 처리량을 처리할 수 있습니다
- 두 애플리케이션 모두 종료 시 리소스를 적절히 정리하도록 설계되었습니다

## 트러블 슈팅

### 일반적인 문제

1. **WebSocket 연결 오류**:
   - 인터넷 연결 확인
   - 네트워크에서 바이낸스 API에 접근 가능한지 확인
   - 지정된 거래 쌍이 바이낸스에서 유효한지 확인

2. **공유 메모리 접근 문제**:
   - 리더를 시작하기 전에 데이터 수집기가 실행 중인지 확인
   - 수집기가 충돌한 경우 `ipcrm`을 사용하여 공유 메모리를 수동으로 정리해야 할 수 있음

3. **컴파일 오류**:
   - 모든 의존성이 설치되어 있는지 확인
   - 헤더 파일이 올바른 위치에 있는지 확인

## 시스템 확장

모듈식 설계로 시스템을 쉽게 확장할 수 있습니다:

- 헤더에 새 데이터 구조를 정의하여 추가 시장 데이터 유형에 대한 지원을 추가할 수 있습니다
- 공유 메모리 형식을 통해 여러 리더가 동시에 데이터에 접근할 수 있습니다
- 이진 데이터 파일을 처리하기 위한 추가 분석 도구를 개발할 수 있습니다

## 라이센스

이 프로젝트는 MIT 라이센스에 따라 라이센스가 부여됩니다 - 자세한 내용은 LICENSE 파일을 참조하세요.

---
