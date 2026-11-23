#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8081
#define CHUNK_SIZE 4096

#define RESPONSE_OK    "OK"
#define RESPONSE_RETRY "RETRY"

#define ABORT_MARKER 0xFFFFFFFFu //client'tan gelen paket ABORT_MARKER ise transfer iptal ediliyor.


#pragma pack(push, 1) //struct'ların bellekteki hizalanmasını 1 byte olarak ayarlıyor. Bu sayede struct'lar arasında padding oluşmaz ve struct'lar tam olarak tanımlandığı gibi bellekte yer kaplar. Bu, ağ üzerinden veri gönderirken önemlidir çünkü alıcı tarafın struct'ı doğru şekilde yorumlamasını sağlar.
typedef struct {   //her paketin basinda bulunan headerlar(packet header)
    uint32_t sequence_number; //paketin sira numarasi
    uint32_t payload_size;  //paketin icindeki veri boyutu
    uint32_t checksum;  //paketin icindeki verinin CRC32 checksum degeri
} PacketHeader;

typedef struct {
    char status[8];
    uint32_t sequence_number;
} ChunkResponse;
#pragma pack(pop)

// *CRC32* hesaplamasi icin fonksiyon
uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// Send all data in chunks until the entire length is sent
int send_all(SOCKET s, const char *data, long long length)
{
    long long sent_total = 0;
    while (sent_total < length) { //send() çağrısı her zaman istediğimiz bütün byte'ları tek seferde göndermek zorunda değil.
        int chunk = (length - sent_total > 65536) ? 65536 : (int)(length - sent_total);
        int sent = send(s, data + sent_total, chunk, 0);
        if (sent == SOCKET_ERROR) return SOCKET_ERROR;
        sent_total += sent;
    }
    return 0;
}

int recv_all(SOCKET s, char *data, long long length)  //recv() çağrısı her zaman istediğimiz bütün byte'ları tek seferde almak zorunda değil. Server'ın istediği miktarda veri tamamen gelene kadar recv() yapıyor.
{
    long long recv_total = 0;
    while (recv_total < length) {
        int chunk = (length - recv_total > 65536) ? 65536 : (int)(length - recv_total);
        int received = recv(s, data + recv_total, chunk, 0);
        if (received <= 0) return SOCKET_ERROR;
        recv_total += received;
    }
    return 0;
}

// --------------------------------------------------
// ENCODE / DECODE the packet header and chunk response
// --------------------------------------------------
// Both the PacketHeader and ChunkResponse structs are explicitly
// ENCODED into a fixed-size wire format using network byte order
// (via htonl/ntohl) before being sent, and explicitly DECODED
// back on receipt, instead of relying on the raw in-memory layout
// of the struct (which only happens to work because both sides
// run on the same little-endian architecture).

#define HEADER_WIRE_SIZE 12
#define RESPONSE_WIRE_SIZE 12

void decode_header(const unsigned char *wire, PacketHeader *header) //Client'tan gelen 12 byte'lık header'ı tekrar PacketHeader yapısına dönüştürüyor.
{
    uint32_t seq_be, size_be, crc_be;

    memcpy(&seq_be, wire, 4);  //3 tane 4 bytelık alanı ayırdı
    memcpy(&size_be, wire + 4, 4);
    memcpy(&crc_be, wire + 8, 4);

    header->sequence_number = ntohl(seq_be);
    header->payload_size = ntohl(size_be);
    header->checksum = ntohl(crc_be);
}

void encode_response(const ChunkResponse *response, unsigned char *wire)  //Server'ın cevabını network üzerinden gönderilebilecek formata çeviriyor.
{
    memcpy(wire, response->status, 8);
    uint32_t seq_be = htonl(response->sequence_number);
    memcpy(wire + 8, &seq_be, 4);
}

int validate_png_structure(const unsigned char *data, long long size)  //bu fonksiyon gelen dosyanın gerçekten yapısal olarak PNG olup olmadığını kontrol ediyor.
{
    static const unsigned char PNG_MAGIC[8] = {  //PNG dosyalarının başında bulunan magic bytes
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (size < 8 || memcmp(data, PNG_MAGIC, 8) != 0) {
        printf("PNG magic bytes invalid.\n");
        return 0;
    }

    long long offset = 8;  
    int saw_ihdr = 0, saw_iend = 0;
    int width = 0, height = 0;

    while (offset + 12 <= size) {  //her chunk 12 byte header + length kadar veri içeriyor. Bu yüzden offset + 12 <= size kontrolü yapılıyor.

        uint32_t length_be;
        memcpy(&length_be, data + offset, 4); //chunk length bilgisini alıyor
        uint32_t length = ntohl(length_be); 

        char type[5];
        memcpy(type, data + offset + 4, 4); //chunk type bilgisini alıyor
        type[4] = '\0'; 

        if (offset + 12 + (long long)length > size) {
            printf("Chunk '%s' declares a size past end of file.\n", type);
            return 0;
        }

        const unsigned char *type_and_data = data + offset + 4;
        long long type_and_data_len = 4 + (long long)length;

        uint32_t stored_crc_be;
        memcpy(&stored_crc_be, data + offset + 8 + length, 4);
        uint32_t stored_crc = ntohl(stored_crc_be);

        uint32_t computed_crc = crc32(type_and_data, (size_t)type_and_data_len); //PNG chunk'ın CRC'sini yeniden hesaplıyor.

        if (computed_crc != stored_crc) {
            printf("Chunk '%s' CRC mismatch (stored=%u computed=%u).\n", type, stored_crc, computed_crc);
            return 0;
        }

        if (strcmp(type, "IHDR") == 0 && length >= 8) { //IHDR chunk'ı PNG dosyalarının başında bulunur ve width ve height bilgilerini içerir.
            uint32_t w_be, h_be;
            memcpy(&w_be, data + offset + 8, 4);
            memcpy(&h_be, data + offset + 8 + 4, 4);
            width = (int)ntohl(w_be);
            height = (int)ntohl(h_be);
            saw_ihdr = 1;
        }

        if (strcmp(type, "IEND") == 0) saw_iend = 1; //IEND chunk'ı PNG dosyalarının sonunda bulunur. PNG'nin sonuna ulaşıldığını gösteriyor.

        offset += 12 + (long long)length;
        if (saw_iend) break;
    }

    if (!saw_ihdr) { printf("No IHDR chunk found - not a valid PNG.\n"); return 0; }
    if (!saw_iend) { printf("No IEND chunk found - file is truncated/corrupt.\n"); return 0; }

    printf("PNG structure valid - %dx%d, all chunk CRCs match.\n", width, height);
    return 1;
}

int main()
{
    WSADATA wsa;
    SOCKET server_socket, client_socket;  //server_socket: serverın ana soketi ve client_socket: Client bağlandıktan sonra onunla iletişim kurulan socket.
    struct sockaddr_in server_address, client_address;
    int client_address_size = sizeof(client_address);

    printf("Starting server...\n");

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0); //TCP soketi oluşturuyoruz. AF_INET: IPv4, SOCK_STREAM: TCP
    if (server_socket == INVALID_SOCKET) {
        printf("Socket creation failed.\n");
        WSACleanup();
        return 1;
    }

    server_address.sin_family = AF_INET; //IPv4
    server_address.sin_addr.s_addr = INADDR_ANY; //serverın tüm IP adreslerinden gelen bağlantıları kabul etmesini sağlıyor.
    server_address.sin_port = htons(PORT); //port numarasını network byte order'a çeviriyor. 8081 portunu kullanıyoruz.

    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {  //server soketini belirtilen IP ve port ile ilişkilendiriyor.
        printf("Bind failed. Error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, 1) == SOCKET_ERROR) { //server soketini dinleme moduna geçiriyor. 1 (Bekleyen bağlantılar için backlog değeri): aynı anda bekleyen bağlantı sayısı
        printf("Listen failed.\n");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    printf("Server is listening on port %d...\n", PORT);

    client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_address_size); //client bağlantısını kabul ediyor. client_socket: client ile iletişim kurmak için kullanılacak yeni soket. client_address: client'ın IP ve port bilgilerini tutar.
    if (client_socket == INVALID_SOCKET) {
        printf("Accept failed.\n");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    printf("Client connected successfully!\n");

    // --------------------------------------------------
    // APPLICATION-LEVEL HANDSHAKE (connection-oriented demo)
    // --------------------------------------------------
    // TCP zaten kendi 3-way handshake'ini (SYN / SYN-ACK / ACK) accept()
    // icinde otomatik yapiyor - ama bu, isletim sistemi seviyesinde
    // gerceklesiyor ve bizim printf ciktilarimizda hic gorunmuyor. Bu
    // baglantinin connection-oriented oldugunu ACIKCA gostermek icin,
    // gercek veri (preamble/chunk) gelmeden once basit bir uygulama-
    // seviyesi "el sikisma" yapiyoruz: client "HELLO" gonderir, server
    // "READY" ile cevap verir. Bu adim olmadan da protokol calisirdi -
    // sadece connection-oriented dogasini gorunur/ispatlanabilir kilmak
    // icin ekleniyor.

    char handshake_recv[8];
    memset(handshake_recv, 0, sizeof(handshake_recv));

    if (recv_all(client_socket, handshake_recv, sizeof(handshake_recv)) == SOCKET_ERROR) {
        printf("Handshake receive failed.\n");
        closesocket(client_socket);
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    printf("[HANDSHAKE] Client'tan '%s' alindi.\n", handshake_recv);

    char handshake_send[8] = "READY";
    if (send_all(client_socket, handshake_send, sizeof(handshake_send)) == SOCKET_ERROR) {
        printf("Handshake send failed.\n");
        closesocket(client_socket);
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    printf("[HANDSHAKE] 'READY' gonderildi. Baglanti kuruldu (connection-oriented).\n\n");

    // Receive the preamble: file size and total chunks
    uint64_t file_size = 0; //client'tan gelen dosya boyutu
    uint32_t total_chunks = 0; //client'tan gelen toplam chunk sayısı

    if (recv_all(client_socket, (char*)&file_size, sizeof(file_size)) == SOCKET_ERROR ||
        recv_all(client_socket, (char*)&total_chunks, sizeof(total_chunks)) == SOCKET_ERROR) {
        printf("Failed to receive preamble.\n");
        closesocket(client_socket);
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    printf("Expecting file: %llu bytes across %u chunks.\n\n",
           (unsigned long long)file_size, total_chunks);

    unsigned char *reassembled = (unsigned char*)malloc((size_t)file_size); //client'tan gelen dosya boyutu kadar bellek ayırıyor. Bu bellek, gelen chunk'lar birleştirilerek dosya haline getirilecek.
    if (reassembled == NULL) {
        printf("Memory allocation failed.\n");
        closesocket(client_socket); //client soketini kapatıyor
        closesocket(server_socket); //server soketini kapatıyor
        WSACleanup(); //Winsock kütüphanesini temizliyor
        return 1;
    }
    memset(reassembled, 0, (size_t)file_size); //ayrılan belleği sıfırlıyor

    unsigned char *packet_buf = (unsigned char*)malloc(CHUNK_SIZE); //Bu ise tek bir chunk'ı geçici olarak tutuyor.

    uint32_t chunks_received = 0; //başarılı bir şekilde alınan chunk sayısı
    long long total_retries_seen = 0; //client'tan gelen RETRY yanıtlarının toplam sayısı
    int transfer_failed = 0; //transfer sırasında bir hata oluşup oluşmadığını belirten bayrak

    while (chunks_received < total_chunks) { //chunk'lar alınana kadar döngü devam ediyor.

        PacketHeader header; //client'tan gelen header bilgilerini tutacak struct
        unsigned char header_wire[HEADER_WIRE_SIZE]; //client'tan gelen 12 byte'lık header'ı tutacak buffer

        if (recv_all(client_socket, (char*)header_wire, HEADER_WIRE_SIZE) == SOCKET_ERROR) {
            printf("Failed to receive packet header.\n");
            transfer_failed = 1;
            break;
        }
        decode_header(header_wire, &header); //header_wire'daki 12 byte'lık veriyi PacketHeader struct'ına dönüştürüyor.

        if (header.sequence_number == ABORT_MARKER) { //client'tan gelen paket ABORT_MARKER ise transfer iptal ediliyor.
            printf("\nClient reported transfer ABORTED (chunk exceeded max retries).\n");
            transfer_failed = 1;
            break;
        }

        if (header.payload_size > CHUNK_SIZE) { //header'daki payload_size, CHUNK_SIZE'dan büyükse bu geçersiz bir durumdur.
            printf("Invalid payload_size %u for packet #%u.\n", header.payload_size, header.sequence_number);
            transfer_failed = 1;
            break;
        }

        if (recv_all(client_socket, (char*)packet_buf, header.payload_size) == SOCKET_ERROR) { //header'daki payload_size kadar veriyi packet_buf'a alıyor.
            printf("Failed to receive payload for packet #%u.\n", header.sequence_number);
            transfer_failed = 1;
            break;
        }

        uint32_t computed_checksum = crc32(packet_buf, header.payload_size); //packet_buf'daki verinin CRC32 checksum değerini hesaplıyor.

        ChunkResponse response; 
        memset(&response, 0, sizeof(response)); 
        response.sequence_number = header.sequence_number; //response struct'ının sequence_number alanını header'daki sequence_number ile dolduruyor.

        if (computed_checksum == header.checksum) { //checksum değerleri eşleşiyorsa paket doğru bir şekilde alınmış demektir.

            long long offset = (long long)header.sequence_number * CHUNK_SIZE; //paketin reassembled dosyasında nereye yerleştirileceğini hesaplıyor. Her chunk CHUNK_SIZE kadar veri içeriyor.
            memcpy(reassembled + offset, packet_buf, header.payload_size); 

            strncpy(response.status, RESPONSE_OK, sizeof(response.status) - 1); //response struct'ının status alanını "OK" olarak ayarlıyor.
            chunks_received++;

            printf("[OK] Paket #%u alindi ve dogrulandi. (%u/%u)\n",
                   header.sequence_number, chunks_received, total_chunks);
        }
        else { //checksum değerleri eşleşmiyorsa paket bozulmuş demektir. Server, client'a RETRY yanıtı gönderiyor.
            strncpy(response.status, RESPONSE_RETRY, sizeof(response.status) - 1);
            total_retries_seen++;

            printf("[HATA] Paket #%u Checksum Uyusmazligi! Retransmission isteniyor... "
                   "(beklenen=%u, hesaplanan=%u)\n",
                   header.sequence_number, header.checksum, computed_checksum);
        }

        unsigned char response_wire[RESPONSE_WIRE_SIZE]; 
        encode_response(&response, response_wire); //response struct'ını network üzerinden gönderilebilecek formata çeviriyor.

        if (send_all(client_socket, (char*)response_wire, RESPONSE_WIRE_SIZE) == SOCKET_ERROR) { //response_wire'daki 12 byte'lık veriyi client'a gönderiyor.
            printf("Failed to send response for packet #%u.\n", header.sequence_number);
            transfer_failed = 1;
            break;
        }
    }

    printf("\n--- Transfer Summary (server side) ---\n");
    printf("Chunks received OK: %u / %u\n", chunks_received, total_chunks);
    printf("Total retry responses sent: %lld\n", total_retries_seen);

    if (!transfer_failed && chunks_received == total_chunks) {

        printf("Transfer status: SUCCESS\n");

        int is_valid_png = validate_png_structure(reassembled, (long long)file_size); //reassembled dosyasının gerçekten yapısal olarak PNG olup olmadığını kontrol ediyor.
        if (!is_valid_png) {
            printf("WARNING: payload is not a structurally valid PNG!\n");
        }

        FILE *output_file = fopen("output/received.png", "wb"); //reassembled dosyasını output/received.png olarak kaydediyor.
        if (output_file != NULL) {
            fwrite(reassembled, 1, (size_t)file_size, output_file); 
            fclose(output_file); 
            printf("Image saved as output/received.png\n");
        } else {
            printf("Could not create output file.\n");
        }
    }
    else {
        printf("Transfer status: FAILED\n");
        printf("(Image not saved -- transfer did not complete successfully.)\n");
    }

    free(reassembled); //reassembled dosyasını tutan belleği serbest bırakıyor
    free(packet_buf); //packet_buf'ı tutan belleği serbest bırakıyor

    closesocket(client_socket); //client soketini kapatıyor
    closesocket(server_socket); //server soketini kapatıyor
    WSACleanup(); //Winsock kütüphanesini temizliyor

    return 0;
}