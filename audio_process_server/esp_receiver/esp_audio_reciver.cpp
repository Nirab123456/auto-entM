//C header includes
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

//cpp header includes 

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// server config (will be upgraded for dynamic controll)
static const char* SELF_LISTENER = "0.0.0.0";
static const uint16_t LISTEN_PORT = 7000; 
static const uint16_t HTTP_PORT = 8080;

//Header Layout 
static const uint32_t HEADER_MAGIC = 0x45535032; // 'ESP2'
static const uint8_t HEADER_SIZE = 34;
static const uint16_t FORMATE_INT32_LEFT24 = 1u;

//audio param 

static const uint32_t SAMPLE_RATE = 48000;
static const uint8_t CHANNELS = 1;
static const uint8_t IN_BYTES_PER_SAMPLE =4;
static const uint8_t OUT_BYTES_PER_SAMPLE = 3;


//filename (will be updraded for dynamuc use)
static const char* OUT_FILENAME = "recived_audio_esp32.wav";

//ring buffer 
static const uint8_t BUFFER_SECONDS = 4;
static const size_t RING_SIZE = (size_t)SAMPLE_RATE * BUFFER_SECONDS;

// global state variables 

std::atomic<bool> global_running(true);
std::atomic<double> global_gain(1.0);
std::mutex global_file_mutex;
int global_total_samples_written = 0;

std::atomic<uint64_t> global_highest_recived_idx(0);
std::atomic<uint32_t> global_last_sequence(0);


bool recive_full (uint16_t socket_file_descriptior, uint8_t* buffer_pointer, size_t required_bytes)
{
    size_t total_recived_bytes = 0;
    while (total_recived_bytes < required_bytes)
    {
        ssize_t current_recived_amount = recv(socket_file_descriptior,buffer_pointer + 
                total_recived_bytes,(required_bytes-total_recived_bytes),0);
        if (current_recived_amount < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            else
            {
                perror("Error in reciving\n");
                return false;
            }
            if (current_recived_amount==0) // 
            {
                return false;
            }
            total_recived_bytes += (size_t)current_recived_amount;   
        }
    }
    return true;
}


void write_uint32t_little_endian(FILE* filename_binary_read_write_access,uint32_t intvalue)
{
    uint8_t store_buffer[4];
    store_buffer[0] = intvalue & 0xff;
    store_buffer[1] = (intvalue>>8) & 0xff;
    store_buffer[2] = (intvalue>>16) &0xff;
    store_buffer[3] = (intvalue>>24) &0xff;
    fwrite(store_buffer,1,4,filename_binary_read_write_access);
}

void write_uint16t_little_endian(FILE* filename_binary_read_write_access,uint32_t intvalue)
{
    uint8_t store_buffer[2];
    store_buffer[0] = intvalue & 0xff;
    store_buffer[1] = (intvalue>>8) & 0xff;
    fwrite(store_buffer,1,2,filename_binary_read_write_access);
}


bool write_wev_header_placeholder(FILE* filename_binary_read_write_access,uint32_t sample_rate, uint8_t channels)
{
    if (!filename_binary_read_write_access)
    {
        return false;
    }

    fwrite("RIFF",1,4,filename_binary_read_write_access);
    write_uint32t_little_endian(filename_binary_read_write_access,16);
    fwrite("WAVE",1,4,filename_binary_read_write_access);
    fwrite("fmt",1,4,filename_binary_read_write_access);
    write_uint32t_little_endian(filename_binary_read_write_access,16);
    write_uint16t_little_endian(filename_binary_read_write_access,1);
    write_uint16t_little_endian(filename_binary_read_write_access,channels);
    uint32_t byte_rate = sample_rate * channels * OUT_BYTES_PER_SAMPLE;
    uint16_t block_align = channels * OUT_BYTES_PER_SAMPLE;
    write_uint32t_little_endian(filename_binary_read_write_access,byte_rate);
    write_uint16t_little_endian(filename_binary_read_write_access,block_align);
    write_uint16t_little_endian(filename_binary_read_write_access,OUT_BYTES_PER_SAMPLE*8);
    fwrite("data",1,4,filename_binary_read_write_access);
    write_uint32t_little_endian(filename_binary_read_write_access,0);
    return true;
    
}


void finalize_wav_header(FILE* filename_binary_read_write_access, uint32_t total_samples,uint8_t channels , uint32_t sample_rate)
{

    uint32_t data_bytes = total_samples * channels * OUT_BYTES_PER_SAMPLE;
    uint32_t riff_size = 4 + (8+16) +(8+data_bytes);
    fflush(filename_binary_read_write_access);
    fseek(filename_binary_read_write_access,4,SEEK_SET);
    write_uint32t_little_endian(filename_binary_read_write_access,riff_size);
    fseek(filename_binary_read_write_access,40,SEEK_SET);
    write_uint32t_little_endian(filename_binary_read_write_access,data_bytes);
    fflush(filename_binary_read_write_access);

}

void int32_to_24_bytes_little_endian(int32_t sound_32_bits,uint8_t out[3])
{
    int32_t sound_24_bits = sound_32_bits >> 8;
    uint32_t casted_sound_24_bits = (uint32_t)sound_24_bits & 0xfffffffu;
    out[0] = casted_sound_24_bits &0xff;
    out[1] = (casted_sound_24_bits>>8)&0xff;
    out[2] = (casted_sound_24_bits>>16)&0xff;
}


inline int32_t little_endian_24b_to_int32_big_endian(const uint8_t* pointer)
{
    uint32_t reconstruction_big_endian = ( (uint32_t)pointer[0] |
                        ((uint32_t)pointer[1]<<8) | ((uint32_t)pointer[2]<<16) |
                        ((uint32_t)pointer[3]<<24)
                                        );

    return (int32_t)reconstruction_big_endian;
}

void validate_header_basics(uint32_t* sample_rate, uint8_t* num_channels, 
    uint8_t* bytes_per_sample, uint16_t* format_id)
{
    if (*sample_rate != SAMPLE_RATE)
    {
        std:: cerr << "Sample rate : " << *sample_rate << " ,dosent match with : " << SAMPLE_RATE << std::endl;
    }
    if (*num_channels != CHANNELS)
    {
        std::cerr << "Recived channels : " << *num_channels << " dosent match with acctual number of channel : " << CHANNELS << std::endl;
    }
    if (*bytes_per_sample != IN_BYTES_PER_SAMPLE)
    {
        std::cerr << "[TCP] warning bytes_per_sample mismatch: " << bytes_per_sample<< " != " << int(IN_BYTES_PER_SAMPLE) << std::endl;
    }
    if (*format_id != FORMATE_INT32_LEFT24) {
        std::cerr << "[TCP] warning format_id mismatch: " << format_id << std::endl;
    }    

    
}


void tcp_server_loop()
{
    int new_socket_file_descriptior = socket(AF_INET,SOCK_STREAM,0);
    if (new_socket_file_descriptior < 0)
    {
        perror("Error creating new socket :(");
        return;
    }
    uint8_t reuse = 1;
    setsockopt(new_socket_file_descriptior,SOL_SOCKET,SO_REUSEADDR, &reuse,sizeof(reuse));

    struct sockaddr_in address_binder;
    address_binder.sin_family = AF_INET;
    address_binder.sin_port = htons(LISTEN_PORT); // will be changed in future for dynamic implimentation
    address_binder.sin_addr.s_addr = INADDR_ANY;

    //binding
    if (bind(new_socket_file_descriptior,
        (struct sockaddr*)&address_binder,
        sizeof(address_binder))<0)
    {
        perror("Error in binding socket");
        close(new_socket_file_descriptior);
        return;
    }
    

    if (listen(new_socket_file_descriptior,1)<0)
    {
        perror("Error in listening");
        close(new_socket_file_descriptior);
        return;
    }

    std::cout << "TCP Connection is established on port" << LISTEN_PORT << std:: endl;

    while (global_running.load())
    {
        struct sockaddr_in client_address_binder; //client ip port 
        socklen_t client_address_len = sizeof(client_address_binder);
        std::cout << "TCP connection waiting for client:"<< std::endl;
        int client_file_descriptior = accept(new_socket_file_descriptior,
             (struct sockaddr*) & client_address_binder, &client_address_len);

        if (client_file_descriptior<0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
            
        }

        //displaying client(esp32 ip address)
        char client_human_readable_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&client_address_binder.sin_addr,
            client_human_readable_ip,sizeof(client_human_readable_ip));
        std::cout << "TCP client connrcted from "<< client_human_readable_ip << ":" <<ntohs(client_address_binder.sin_port) <<std::endl;


        FILE* output_file_read_write_binary = nullptr;
        {
            std::lock_guard<std::mutex> lk (global_file_mutex);
            output_file_read_write_binary = fopen(OUT_FILENAME,"wb+");
            if (!output_file_read_write_binary)
            {
                perror("Error in opening the file");
                close(client_file_descriptior);
                continue;
            }

            //write wev header placeholder
            if (!write_wev_header_placeholder(output_file_read_write_binary,SAMPLE_RATE,CHANNELS));
            {
                std::cerr << "wav header write failed" << std:: endl;
                fclose(output_file_read_write_binary);
                close(client_file_descriptior);
                continue;
            }
            
            fseek(output_file_read_write_binary,0,SEEK_END);

            bool connection_ok = true;

            uint8_t header[HEADER_SIZE];

            while (global_running.load()&&connection_ok)
            {
                if (!recive_full(client_file_descriptior,header,HEADER_SIZE))
                {
                    std::cout << "TCP header didn't recived connection failed" << std:: endl;
                    break;
                }
                

                uint32_t header_magic_reconstruction = (
                    (uint32_t)header[0]|((uint32_t)header[1]<<8)|
                    ((uint32_t)header[2]<<16|((uint32_t)header[3]<<24))
                );

                if (header_magic_reconstruction != HEADER_MAGIC)
                {
                    std::cerr<<"TCP bad magic : " << std::hex << header_magic_reconstruction << std::dec << std::endl;
                    connection_ok = false;
                    break;
                }

                uint32_t packet_sequence = ((uint32_t)header[4] | ((uint32_t)header[5]<<8)|
                                            ((uint32_t)header[5]<<16 | ((uint32_t)header[7]<<24))
            );

                uint64_t first_sample_index = 0;

                for (size_t i = 0; i < 8; i++)
                {
                    first_sample_index |= ((uint64_t)header[8+i]<<(8*i));
                }


                uint64_t universal_timestamp = 0;
                for (size_t i = 0; i < 8; i++)
                {
                    universal_timestamp |= ((uint64_t)header[8+i]<<(8*i));
                }

                uint16_t number_of_frames = (
                    ((uint16_t)header[24]) | ((uint16_t)header[25]<<8)
                );

                uint8_t number_of_channels = header[26];

                uint8_t bytes_per_sample = header[27];

                uint32_t sample_rate = (
                    (uint32_t)header[28] | ((uint32_t)header[29]<<8) |
                    ((uint32_t)header[30]<<16) | ((uint32_t)header[31]<<24)
                );

                uint16_t format_id = (
                    (uint16_t)header[32] | ((uint16_t)header[33]<<8)
                );

                validate_header_basics(&sample_rate,&number_of_channels,
                        &bytes_per_sample,&format_id);

                size_t payload_bytes = (
                    (size_t)number_of_frames * (size_t) number_of_channels*
                    (size_t) bytes_per_sample
                );
                if (payload_bytes == 0)
                {
                    std::cerr << "No payload recived"<< std::endl;
                    continue;
                }

                std::vector<uint8_t> payload_data_container(payload_bytes);
                if (!recive_full(client_file_descriptior,payload_data_container.data(),payload_bytes))
                {
                    std::cerr << "Failure in reciving payload but might recived payload of : " << payload_bytes << " bytes" << std::endl;
                }
                

                std::vector<uint8_t> out_bytes_container;
                out_bytes_container.reserve((size_t)number_of_frames *OUT_BYTES_PER_SAMPLE);
                for (size_t i = 0; i < (size_t)number_of_frames; i++)
                {
                    size_t base_shift = i*4;
                    int32_t sound_reconstruction_little_endian_int32 = (int32_t)(
                        ((uint32_t)payload_data_container[base_shift+0]) |
                        ((uint32_t)payload_data_container[base_shift+1]<<8) |
                        ((uint32_t)payload_data_container[base_shift+2]<<16) |
                        ((uint32_t)payload_data_container[base_shift+3]<<24)
                    );
                    double gain_factor = global_gain.load();
                    double scaled_reconstruction = (double)sound_reconstruction_little_endian_int32 * gain_factor;

                    if (scaled_reconstruction > (double)INT32_MAX)
                    {
                        scaled_reconstruction = (double)INT32_MAX;
                    }
                    else if (scaled_reconstruction < (double)INT32_MIN)
                    {
                        scaled_reconstruction = (double)INT32_MIN;
                    }

                    int32_t scaled_sound_reconstruction_little_endian_int32 = (int32_t) std::llround(scaled_reconstruction);

                    int32_t scaled_sound_reconstruction_little_endian_24bit = scaled_sound_reconstruction_little_endian_int32 >>8;
                    uint32_t unsigned_24bit_masked_sound = ((uint32_t)scaled_sound_reconstruction_little_endian_24bit & 0xFFFFFFu);
                    out_bytes_container.push_back((uint8_t)(unsigned_24bit_masked_sound & 0xff));
                    out_bytes_container.push_back((uint32_t)(unsigned_24bit_masked_sound >> 8) & 0xff);
                    out_bytes_container.push_back((uint32_t)(unsigned_24bit_masked_sound >> 16) & 0xff);
                    
                }
                
                {
                    std::lock_guard<std::mutex> write_lock(global_file_mutex);
                    size_t written = fwrite(
                        out_bytes_container.data(),1,
                        out_bytes_container.size(),output_file_read_write_binary
                    );
                    if (written != out_bytes_container.size())
                    {
                        std::cerr<< "Short write (written) : " << written << " out of : " << out_bytes_container.size() << std::endl;
                    }
                    fflush(output_file_read_write_binary);
                    global_total_samples_written += (int)number_of_frames;
                }

                global_highest_recived_idx.store(first_sample_index+number_of_frames-1);
                global_last_sequence.store(packet_sequence);

            }
            close(client_file_descriptior);
            
            std::cout << "[TCP] client disconnected, continuing listen" << std::endl;
            if (output_file_read_write_binary)
            {
                std::lock_guard<std::mutex>write_lock(global_file_mutex);
                fclose(output_file_read_write_binary);
                output_file_read_write_binary = nullptr;
            }
            
        }
        
    }
    
    close(new_socket_file_descriptior);
    std::cout<<"Exiting from TCP server"<<std::endl;

    
}




int main()
{
    return 0;
}