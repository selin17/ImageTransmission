#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8081
#define CHUNK_SIZE 4096

// Noise level in percentage
#define NOISE_PERCENT 20.0

#define INTRA_CHUNK_FLIP_PROB 0.10 //%10 ihtimalle bit ters çevriliyor.
#define MAX_RETRIES 5

#define RESPONSE_OK    "OK"
#define RESPONSE_RETRY "RETRY"

#define ABORT_MARKER 0xFFFFFFFFu  //Client tarafından gönderilen ve sunucunun tüm transferi iptal etmesini tetikleyen özel bir paket numarası.


#pragma pack(push, 1) //Ensure no padding in the struct
typedef struct {
    uint32_t sequence_number; //chunkın sırasını belirten numara
    uint32_t payload_size;  //chunkın boyutunu belirten numara. Pakette kaç byte veri olduğunu söyler.
    uint32_t checksum; //chunkın CRC32 checksum değerini belirten numara
} PacketHeader;

typedef struct {
    char status[8]; // "OK" veya "RETRY" gibi yanıt durumunu belirten dize
    uint32_t sequence_number; //chunkın sırasını belirten numara
} ChunkResponse;
#pragma pack(pop) //Restore default packing


uint32_t crc32(const unsigned char *data, size_t length) // CRC32 hesaplaması için kullanılan fonksiyon
{
    uint32_t crc = 0xFFFFFFFF; // Başlangıç değeri
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

int send_all(SOCKET s, const char *data, long long length) // Tüm veriyi göndermek için kullanılan fonksiyon
{
    long long sent_total = 0;
    while (sent_total < length) { // Maksimum 64KB'lık parçalar halinde gönderim yapılır
        int chunk = (length - sent_total > 65536) ? 65536 : (int)(length - sent_total);
        int sent = send(s, data + sent_total, chunk, 0);
        if (sent == SOCKET_ERROR) return SOCKET_ERROR; // Hata durumunda SOCKET_ERROR döndürülür
        sent_total += sent;
    }
    return 0;
}

int recv_all(SOCKET s, char *data, long long length) // Tüm veriyi almak için kullanılan fonksiyon
{
    long long recv_total = 0;
    while (recv_total < length) { //12 byte tamamlanana kadar alınır. 
        int chunk = (length - recv_total > 65536) ? 65536 : (int)(length - recv_total); // Maksimum 64KB'lık parçalar halinde alım yapılır
        int received = recv(s, data + recv_total, chunk, 0); // Veriyi almak için recv fonksiyonu kullanılır
        if (received <= 0) return SOCKET_ERROR; // Hata durumunda SOCKET_ERROR döndürülür
        recv_total += received;
    }
    return 0;
}

// --------------------------------------------------
// ENCODE / DECODE the packet header
// --------------------------------------------------
// Rather than sending the PacketHeader struct's raw memory
// bytes as-is (which only works because both sides happen to
// run on the same little-endian architecture with the same
// struct padding), the header is explicitly ENCODED into a
// fixed 12-byte wire format using network byte order (via
// htonl), and the receiver explicitly DECODES it back. This
// is the standard approach used by real network protocols so
// that the wire format never depends on either machine's
// internal memory layout.

#define HEADER_WIRE_SIZE 12 // 4 bytes for sequence_number + 4 bytes for payload_size + 4 bytes for checksum

void encode_header(const PacketHeader *header, unsigned char *wire) // PacketHeader struct'ını network üzerinden gönderilebilecek formata çeviriyor.
{
    uint32_t seq_be = htonl(header->sequence_number);
    uint32_t size_be = htonl(header->payload_size);
    uint32_t crc_be = htonl(header->checksum);

    memcpy(wire, &seq_be, 4);
    memcpy(wire + 4, &size_be, 4);
    memcpy(wire + 8, &crc_be, 4);
}


void decode_header(const unsigned char *wire, PacketHeader *header) //clienttan  gelen 12 bytelık headerı tekrar PacketHeader yapısına dönüştürüyor. 
{
    uint32_t seq_be, size_be, crc_be; //3 tane 4 bytelık alanı ayırdı

    memcpy(&seq_be, wire, 4); 
    memcpy(&size_be, wire + 4, 4); 
    memcpy(&crc_be, wire + 8, 4);

    header->sequence_number = ntohl(seq_be); //network byte order'dan host byte order'a çeviriyor.
    header->payload_size = ntohl(size_be); 
    header->checksum = ntohl(crc_be); 
}

#define RESPONSE_WIRE_SIZE 12 // 8 bytes for status + 4 bytes for sequence_number

void encode_response(const ChunkResponse *response, unsigned char *wire) //Server'ın cevabını network üzerinden gönderilebilecek formata çeviriyor.
{
    memcpy(wire, response->status, 8); 
    uint32_t seq_be = htonl(response->sequence_number); //sequence_number'ı network byte order'a çeviriyor.
    memcpy(wire + 8, &seq_be, 4);
}

void decode_response(const unsigned char *wire, ChunkResponse *response) //Server'dan gelen 12 bytelık cevabı tekrar ChunkResponse yapısına dönüştürüyor.
{
    memset(response, 0, sizeof(*response)); //response struct'ını sıfırlıyor.
    memcpy(response->status, wire, 8); //status alanını wire'dan alıyor.
    uint32_t seq_be;
    memcpy(&seq_be, wire + 8, 4);
    response->sequence_number = ntohl(seq_be);
}

void corrupt_chunk( //chunk'ı bozan fonksiyon. Yapay noise eklemek için kullanılıyor.
    const unsigned char *clean,
    unsigned char *out,
    int size,
    long long *total_bits,
    long long *flipped_bits,
    double *signal_power_sum,
    double *noise_power_sum
)
{
    for (int i = 0; i < size; i++) { //Chunk'ın içerisindeki bütün byte'lar dolaşılıyor.

        unsigned char original = clean[i];
        unsigned char noisy = original;

        for (int bit = 0; bit < 8; bit++) { //Her byte'ın içerisindeki bütün bitler dolaşılıyor.

            if (total_bits) (*total_bits)++;

            int original_bit = (original >> bit) & 1;
            if (signal_power_sum) *signal_power_sum += (double)(original_bit * original_bit);

            double r = (double)rand() / RAND_MAX; //0.0 ile 1.0 arasında rastgele sayı üretiyor. Noise eklemek için kullanılıyor.
            if (r < INTRA_CHUNK_FLIP_PROB) {
                noisy ^= (1 << bit); //Bit'i ters çeviriyor.
                if (flipped_bits) (*flipped_bits)++; //Ters çevrilen bit sayısını artırıyor.
                if (noise_power_sum) *noise_power_sum += 1.0; //Ters çevrilen bitin gücünü noise_power_sum'a ekliyor.
            }
        }

        out[i] = noisy;
    }
}

int main() // Programın ana fonksiyonu
{
    WSADATA wsa; //socket kütüphanesini başlatmak için kullanılan yapı
    SOCKET client_socket; //socket oluşturmak için kullanılan değişken
    struct sockaddr_in server_address; //socket adresini tutmak için kullanılan yapı

    printf("Starting client...\n");
    printf("Noise level (per-chunk corruption probability): %.2f%%\n", NOISE_PERCENT); 

    if (NOISE_PERCENT < 0.0 || NOISE_PERCENT > 100.0) { //Noise yüzdesi 0 ile 100 arasında olmalı. Eğer değilse hata mesajı veriyor ve programdan çıkıyor.
        printf("ERROR: Noise percentage must be between 0 and 100.\n");
        return 1;
    }

    srand((unsigned int)time(NULL)); //Rastgele sayı üretmek için kullanılan seed değerini ayarlıyor.

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { //socket kütüphanesini başlatıyor. Eğer başarısız olursa hata mesajı veriyor ve programdan çıkıyor. Bunu yapmadan socket işlemleri yapılamaz.
        printf("WSAStartup failed.\n");
        return 1;
    }

    client_socket = socket(AF_INET, SOCK_STREAM, 0); //socket oluşturuyor. AF_INET: IPv4, SOCK_STREAM: TCP, 0: default protocol
    if (client_socket == INVALID_SOCKET) {
        printf("Socket creation failed.\n"); //Eğer başarısız olursa hata mesajı veriyor ve programdan çıkıyor.
        WSACleanup(); //socket kütüphanesini temizliyor ve programdan çıkıyor.
        return 1;
    }

    server_address.sin_family = AF_INET; //IPv4
    server_address.sin_addr.s_addr = inet_addr("127.0.0.1"); //localhost
    server_address.sin_port = htons(PORT); //port numarası. 8081

    if (connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) { //server'a bağlanmaya çalışıyor. Eğer başarısız olursa hata mesajı veriyor ve programdan çıkıyor.
        printf("Connection failed.\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    printf("Connected to server successfully!\n");

    // --------------------------------------------------
    // APPLICATION-LEVEL HANDSHAKE (connection-oriented demo)
    // --------------------------------------------------
    // TCP zaten kendi 3-way handshake'ini (SYN / SYN-ACK / ACK) connect()
    // icinde otomatik yapiyor - ama bu, isletim sistemi seviyesinde
    // gerceklesiyor ve bizim printf ciktilarimizda hic gorunmuyor. Bu
    // baglantinin connection-oriented oldugunu ACIKCA gostermek icin,
    // gercek veri (preamble/chunk) gonderilmeden once basit bir uygulama-
    // seviyesi "el sikisma" yapiyoruz: client "HELLO" gonderir, server
    // "READY" ile cevap verir. Bu adim olmadan da protokol calisirdi -
    // sadece connection-oriented dogasini gorunur/ispatlanabilir kilmak
    // icin ekleniyor.

    char handshake_send[8] = "HELLO";
    char handshake_recv[8];
    memset(handshake_recv, 0, sizeof(handshake_recv));

    if (send_all(client_socket, handshake_send, sizeof(handshake_send)) == SOCKET_ERROR) {
        printf("Handshake send failed.\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    printf("[HANDSHAKE] 'HELLO' gonderildi, sunucudan yanit bekleniyor...\n");

    if (recv_all(client_socket, handshake_recv, sizeof(handshake_recv)) == SOCKET_ERROR) {
        printf("Handshake receive failed.\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    printf("[HANDSHAKE] Sunucudan '%s' alindi. Baglanti kuruldu (connection-oriented).\n\n", handshake_recv);

    FILE *input_file = fopen("input/image.png", "rb"); //input klasöründeki image.png dosyasını binary modda açıyor. Eğer dosya yoksa hata mesajı veriyor ve programdan çıkıyor.
    if (input_file == NULL) {
        printf("Could not open input image.\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    _fseeki64(input_file, 0, SEEK_END); //Dosyanın sonuna gidiyor ve dosya boyutunu öğrenmek için kullanılıyor.
    long long file_size = _ftelli64(input_file); //Dosya boyutunu öğreniyor.
    _fseeki64(input_file, 0, SEEK_SET); //Dosyanın başına geri dönüyor.

    printf("Image size: %lld bytes\n", file_size);

    unsigned char *original_image = (unsigned char*)malloc(file_size); //Dosya boyutuna göre bellek ayırıyor. Eğer bellek ayırma başarısız olursa hata mesajı veriyor ve programdan çıkıyor.
    if (original_image == NULL) { 
        printf("Memory allocation failed.\n");
        fclose(input_file);
        closesocket(client_socket); 
        WSACleanup();
        return 1;
    }

    if (fread(original_image, 1, file_size, input_file) != (size_t)file_size) { //Dosyayı okuyor. Eğer dosya boyutu kadar okunamazsa hata mesajı veriyor ve programdan çıkıyor.
        printf("Failed to read complete image.\n");
        fclose(input_file); //Dosyayı kapatıyor.
        free(original_image); //Belleği boşaltıyor.
        closesocket(client_socket); //Socket'i kapatıyor.
        WSACleanup(); //socket kütüphanesini temizliyor.
        return 1; //Programdan çıkıyor.
    }
    fclose(input_file);

    //*Orjinal fileın CRC32 checksum değeri* Calculate the original file checksum. *CRC32* hesaplaması yapılıyor. Bu checksum, dosyanın bütünlüğünü doğrulamak için kullanılacak.
    uint32_t original_file_checksum = crc32(original_image, file_size); 
    printf("Original file checksum: %u\n", original_file_checksum); //csv dosyasına yazmak için

    uint32_t total_chunks = (uint32_t)((file_size + CHUNK_SIZE - 1) / CHUNK_SIZE); //Dosyanın kaç chunk'a bölüneceğini hesaplıyor. Dosya boyutunu chunk boyutuna bölüyor ve kalan varsa bir chunk daha ekliyor.
    printf("Total chunks: %u (chunk size %d bytes)\n\n", total_chunks, CHUNK_SIZE);

    uint64_t network_file_size = (uint64_t)file_size; //Server'a göndereceğimiz dosya boyutu. 64 bit unsigned integer olarak tanımlanıyor.

    if (send_all(client_socket, (char*)&network_file_size, sizeof(network_file_size)) == SOCKET_ERROR ||
        send_all(client_socket, (char*)&total_chunks, sizeof(total_chunks)) == SOCKET_ERROR) {
        printf("Failed to send preamble.\n");
        free(original_image);
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    //***Bit error rate (BER) ve signal-to-noise ratio (SNR) hesaplamaları için gerekli değişkenler***
    long long total_bits = 0; //Toplam bit sayısı
    long long flipped_bits = 0; //Ters çevrilen bit sayısı
    double signal_power_sum = 0.0; //Signal gücünün toplamı
    double noise_power_sum = 0.0; //Noise gücünün toplamı

    long long total_retransmissions = 0; //Toplam retransmission sayısı
    long long chunks_that_needed_retry = 0; //En az bir kez retry gerektiren chunk sayısı

    // NEW: which chunk (if any) exceeded MAX_RETRIES and caused
    // the whole transfer to fail, and how many attempts it took
    // (always MAX_RETRIES+1 by design, but useful to log plainly).
    int32_t failed_chunk_seq = -1; //Failed olan chunk'ın sırasını tutan değişken. Başlangıçta -1 olarak ayarlanıyor, yani henüz başarısız bir chunk yok.
    int failed_chunk_attempts = 0; //Failed olan chunk'ın kaç denemede başarısız olduğunu tutan değişken. Başlangıçta 0 olarak ayarlanıyor.

    unsigned char chunk_buf[CHUNK_SIZE]; //(Gönderilecek)Chunk verilerini tutmak için kullanılan buffer. Chunk boyutu kadar bellek ayırıyor.

    int transfer_failed = 0; //Transferin başarısız olup olmadığını tutan değişken. Başlangıçta 0 olarak ayarlanıyor, yani transfer başarılı.

    for (uint32_t seq = 0; seq < total_chunks; seq++) { //Tüm chunk'lar için döngü başlatılıyor.

        long long offset = (long long)seq * CHUNK_SIZE; //Chunk'ın dosya içindeki başlangıç offset'ini hesaplıyor. Offset, dosyada o chunk'ın başlangıç konumudur.
        int payload_size = (int)((file_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (file_size - offset)); //Chunk'ın payload boyutunu hesaplıyor. Eğer kalan dosya boyutu chunk boyutundan büyükse chunk boyutu kadar, değilse kalan dosya boyutu kadar olacak.

        uint32_t expected_checksum = crc32(original_image + offset, payload_size); //*Chunk'ın CRC32 checksum* değerini hesaplıyor. Bu checksum, chunk'ın bütünlüğünü doğrulamak için kullanılacak.

        int chunk_success = 0; //Chunk'ın başarılı bir şekilde gönderilip gönderilmediğini tutan değişken. Başlangıçta 0 olarak ayarlanıyor, yani chunk henüz başarılı değil.
        int attempt = 0; // 0 = first try, 1..MAX_RETRIES = retries

        while (attempt <= MAX_RETRIES) { //Chunk'ı göndermek için deneme sayısını kontrol eden döngü başlatılıyor. Maksimum deneme sayısı MAX_RETRIES olarak belirlenmiş.

            //Her denemede rastgele bir r üretiliyor ve bu r, NOISE_PERCENT/100.0 değerinden küçükse chunk bozulacak. Aksi takdirde chunk bozulmayacak.
            double r = (double)rand() / RAND_MAX; //0.0 ile 1.0 arasında rastgele sayı üretiyor. Bu sayı, chunk'ın bozulup bozulmayacağını belirlemek için kullanılacak.
            int this_attempt_corrupted = (r < (NOISE_PERCENT / 100.0)); //Eğer rastgele sayı, NOISE_PERCENT/100.0 değerinden küçükse, bu denemede chunk bozulacak. Aksi takdirde chunk bozulmayacak.

            if (this_attempt_corrupted) { //Chunk bozulacaksa, corrupt_chunk fonksiyonu çağrılıyor ve chunk verileri bozuluyor. Bu fonksiyon, chunk'ın orijinal verilerini alıyor ve belirli bir olasılıkla bitleri ters çevirerek bozulmuş bir versiyonunu oluşturuyor.
                corrupt_chunk(
                    original_image + offset, chunk_buf, payload_size, //Orjinal chunk verilerini alıyor ve bozulmuş chunk verilerini chunk_buf'a yazıyor.
                    (attempt == 0) ? &total_bits : NULL, //Sadece ilk denemede toplam bit sayısını güncelliyor. Diğer denemelerde NULL geçiliyor.
                    (attempt == 0) ? &flipped_bits : NULL, //Sadece ilk denemede ters çevrilen bit sayısını güncelliyor. Diğer denemelerde NULL geçiliyor.
                    (attempt == 0) ? &signal_power_sum : NULL, //Sadece ilk denemede sinyal gücü toplamını güncelliyor. Diğer denemelerde NULL geçiliyor.
                    (attempt == 0) ? &noise_power_sum : NULL //Sadece ilk denemede gürültü gücü toplamını güncelliyor. Diğer denemelerde NULL geçiliyor.
                );
            } else {  //Chunk bozulmayacaksa, orijinal chunk verileri chunk_buf'a kopyalanıyor. Bu, bozulmamış bir chunk göndermek için yapılır.
                memcpy(chunk_buf, original_image + offset, payload_size);
                if (attempt == 0) {
                    for (int i = 0; i < payload_size; i++) {
                        unsigned char b = original_image[offset + i];
                        for (int bit = 0; bit < 8; bit++) {
                            total_bits++;
                            int ob = (b >> bit) & 1;
                            signal_power_sum += (double)(ob * ob);
                        }
                    }
                }
            }

            PacketHeader header; //Gönderilecek paket header'ını tutan değişken. Bu header, chunk'ın sırasını, boyutunu ve checksum değerini içeriyor.
            header.sequence_number = seq;
            header.payload_size = (uint32_t)payload_size;
            header.checksum = expected_checksum;

            unsigned char header_wire[HEADER_WIRE_SIZE]; //Gönderilecek paket header'ını network üzerinden gönderilebilecek formata çeviriyor. Bu, header'ın raw memory bytes'ını göndermek yerine, network byte order'a çevirerek gönderilmesini sağlıyor.
            encode_header(&header, header_wire); //header'ı network byte order'a çeviriyor.

            printf(
                "[GONDERILDI] Paket #%u - Boyut: %d bayt - Noise: %%%.0f%s\n",
                seq, payload_size, NOISE_PERCENT,
                this_attempt_corrupted ? " (bozuldu)" : ""
            );

            if (send_all(client_socket, (char*)header_wire, HEADER_WIRE_SIZE) == SOCKET_ERROR || //Gönderilecek paket header'ını gönderiyor. Eğer başarısız olursa hata mesajı veriyor ve transfer_failed değişkenini 1 yapıyor.
                send_all(client_socket, (char*)chunk_buf, payload_size) == SOCKET_ERROR) {
                printf("Send failed for packet #%u.\n", seq);
                transfer_failed = 1;
                break;
            }

            ChunkResponse response; //Server'dan gelen cevabı tutan değişken. Bu response, server'ın chunk'ı başarılı bir şekilde alıp almadığını belirten bir status ve chunk'ın sırasını içeriyor.
            memset(&response, 0, sizeof(response)); //response struct'ını sıfırlıyor.

            unsigned char response_wire[RESPONSE_WIRE_SIZE]; //Server'dan gelen cevabı network üzerinden alınabilecek formata çeviriyor. Bu, response'ın raw memory bytes'ını almak yerine, network byte order'a çevirerek almasını sağlıyor.

            if (recv_all(client_socket, (char*)response_wire, RESPONSE_WIRE_SIZE) == SOCKET_ERROR) { //Server'dan gelen cevabı alıyor. Eğer başarısız olursa hata mesajı veriyor ve transfer_failed değişkenini 1 yapıyor.
                printf("Failed to receive response for packet #%u.\n", seq);
                transfer_failed = 1;
                break;
            }
            decode_response(response_wire, &response); //Server'dan gelen cevabı network byte order'dan host byte order'a çeviriyor.

            if (strcmp(response.status, RESPONSE_OK) == 0) { //Server'dan gelen cevabın status'u *"OK"* ise, chunk başarılı bir şekilde gönderilmiş demektir. Bu durumda chunk_success değişkenini 1 yapıyor ve döngüden çıkıyor.
                chunk_success = 1;
                break;
            }

            // RETRY
            printf("[HATA] Paket #%u Checksum Uyusmazligi! Retransmission isteniyor...\n", seq); //Server'dan gelen cevabın status'u *"RETRY"* ise, chunk başarısız bir şekilde gönderilmiş demektir. Bu durumda attempt değişkenini artırıyor ve döngüye devam ediyor.
            attempt++; //Retry sayısını artırıyor.
            total_retransmissions++;
            if (attempt == 1) chunks_that_needed_retry++;
        }

        if (transfer_failed) break; //Eğer transfer_failed değişkeni 1 ise, döngüden çıkıyor. Bu, chunk gönderimi sırasında bir hata oluştuğunu gösteriyor.

        if (!chunk_success) { //Eğer chunk_success değişkeni 0 ise, chunk başarısız bir şekilde gönderilmiş demektir. Bu durumda transfer_failed değişkenini 1 yapıyor ve döngüden çıkıyor.
            printf(
                "[BASARISIZ] Paket #%u, %d denemeden sonra hala hatali. Iletim iptal ediliyor.\n",
                seq, MAX_RETRIES + 1 //deneme sayısı (ilk deneme + MAX_RETRIES)
            );
            transfer_failed = 1;

            // NEW: record which chunk caused the failure and
            // after how many attempts (first try + retries).
            failed_chunk_seq = (int32_t)seq; //Failed olan chunk'ın sırasını tutan değişken.
            failed_chunk_attempts = MAX_RETRIES + 1;

            PacketHeader abort_header; //Sunucuya transferin iptal edildiğini bildiren özel bir paket header'ı oluşturuyor. Bu header, sequence_number alanına ABORT_MARKER değerini atıyor ve payload_size ve checksum alanlarını 0 yapıyor.
            abort_header.sequence_number = ABORT_MARKER; //Sunucuya transferin iptal edildiğini bildiren özel bir paket numarası. Bu, sunucunun tüm transferi iptal etmesini tetikleyen özel bir paket numarasıdır.
            abort_header.payload_size = 0; //Sunucuya transferin iptal edildiğini bildiren özel bir paket payload boyutu. Bu, sunucunun tüm transferi iptal etmesini tetikleyen özel bir paket numarasıdır.
            abort_header.checksum = 0; //Sunucuya transferin iptal edildiğini bildiren özel bir paket checksum değeri. Bu, sunucunun tüm transferi iptal etmesini tetikleyen özel bir paket numarasıdır.

            unsigned char abort_wire[HEADER_WIRE_SIZE];
            encode_header(&abort_header, abort_wire);
            send_all(client_socket, (char*)abort_wire, HEADER_WIRE_SIZE);

            break;
        }
    }

    //BER ve SNR hesaplamaları
    double actual_ber = (total_bits > 0) ? ((double)flipped_bits / total_bits) * 100.0 : 0.0; //BER = flipped_bits / total_bits × 100

    double snr;  //SNR = 10 * log10(signal_power / noise_power) //SNR yüksek → sinyal daha temiz, SNR düşük → gürültü daha fazla
    if (noise_power_sum > 0.0) {
        double signal_power = signal_power_sum / total_bits;
        double noise_power = noise_power_sum / total_bits;
        snr = 10.0 * log10(signal_power / noise_power);
    } else {
        snr = INFINITY;
    }

    printf("\n--- Transfer Summary ---\n");
    printf("Total chunks: %u\n", total_chunks);
    printf("Chunks that needed at least one retry: %lld\n", chunks_that_needed_retry);
    printf("Total retransmissions: %lld\n", total_retransmissions);
    printf("Actual BER (first attempt): %.6f%%\n", actual_ber);
    if (isinf(snr)) printf("SNR: INF dB\n");
    else printf("SNR: %.6f dB\n", snr);
    printf("Transfer status: %s\n", transfer_failed ? "FAILED" : "SUCCESS");
    if (transfer_failed && failed_chunk_seq >= 0) { //transfer_failed 1 ise FAILED
        printf("Failed at packet #%d after %d attempts.\n", failed_chunk_seq, failed_chunk_attempts);
    }

    // --------------------------------------------------
    // Save results to CSV
    // --------------------------------------------------

    FILE *results_file = fopen("results.csv", "a"); //a append demek. Eğer dosya yoksa oluşturur, varsa yeni satır ekler.
    if (results_file != NULL) {
 
        fseek(results_file, 0, SEEK_END); //Dosyanın sonuna gidiyor. Dosya boşsa başlıkları yazacak, değilse sadece verileri yazacak.
        if (ftell(results_file) == 0) {
            fprintf(results_file, //csv başlıkları
                "Noise_Percent,Total_Chunks,Chunks_Retried,Total_Retransmissions,"
                "Actual_BER,SNR_dB,Original_Checksum,Transfer_Status,"
                "Failed_Chunk_Seq,Failed_Chunk_Attempts\n"
            );
        }

        if (failed_chunk_seq >= 0) { //Failed olan chunk varsa, hem verileri hem de failed chunk bilgilerini yazacak.
            fprintf(results_file,
                "%.2f,%u,%lld,%lld,%.6f,%.6f,%u,%s,%d,%d\n",
                NOISE_PERCENT, total_chunks, chunks_that_needed_retry, total_retransmissions,
                actual_ber, snr, original_file_checksum,
                transfer_failed ? "FAILED" : "SUCCESS",
                failed_chunk_seq, failed_chunk_attempts
            );
        } else {
            fprintf(results_file, //Failed olan chunk yoksa, sadece verileri yazacak.
                "%.2f,%u,%lld,%lld,%.6f,%.6f,%u,%s,,\n",
                NOISE_PERCENT, total_chunks, chunks_that_needed_retry, total_retransmissions,
                actual_ber, snr, original_file_checksum,
                transfer_failed ? "FAILED" : "SUCCESS"
            );
        }

        fclose(results_file); //Dosyayı kapatıyor.
        printf("Experiment result saved to results.csv\n");
    }

    free(original_image); //Orjinal image için ayrılan belleği boşaltıyor.
    shutdown(client_socket, SD_BOTH); //Socket'i kapatıyor. SD_BOTH: hem gönderme hem de alma işlemlerini kapatıyor.
    closesocket(client_socket); //Socket'i kapatıyor.
    WSACleanup(); //socket kütüphanesini temizliyor.

    return 0;
}