#include"Buffer.h"
//buffer



//bufferblock
static const size_t INITIAL_BLOCK_SIZE = 2048; // 2KB
static const size_t MAX_BLOCK_SIZE = 64 * 1024; // 64KB

BufferBlock::BufferBlock():total_size_(0),read_pos_(0),write_pos_(0),check_pos_(0){
  blocks_.emplace_back(INITIAL_BLOCK_SIZE);
}

BufferBlock::~BufferBlock()=default;
void BufferBlock::append(std::string&str){
  append(str.data(),str.size());
}

BufferBlock::BufferBlock(BufferBlock&& rhs)noexcept
:total_size_(rhs.total_size_),read_pos_(rhs.read_pos_),write_pos_(rhs.write_pos_),check_pos_(rhs.check_pos_),
blocks_(std::move(rhs.blocks_)){
  rhs.total_size_ = 0;
  rhs.read_pos_ = 0;
  rhs.write_pos_ = 0;
  rhs.check_pos_ = 0;
}

BufferBlock& BufferBlock::operator=(BufferBlock&&rhs)noexcept {
  if(&rhs!=this){
    blocks_.clear();

    total_size_=rhs.total_size_;
    read_pos_=rhs.read_pos_;
    write_pos_=rhs.write_pos_;
    check_pos_=rhs.check_pos_;
    blocks_= std::move(rhs.blocks_);

    rhs.total_size_ = 0;
    rhs.read_pos_ = 0;
    rhs.write_pos_ = 0;
    rhs.check_pos_ = 0;
  }
  return *this;
}
//将数据追加到buf_中
void BufferBlock::append(const char* data,size_t size){
  if(size==0) return;

  if (blocks_.empty()) {
    blocks_.emplace_back(INITIAL_BLOCK_SIZE);
    write_pos_ = 0;
  }
  size_t remaining = size;
  const char * src = data;

  while(remaining > 0){
    //读取最后一个内存块
    Block& current = blocks_.back();
    //判断其可以写入的字节数
    size_t available = current.capacity - write_pos_;

    //如果为0，则需要在申请一个内存块
    if(available == 0){
      size_t new_size = std::min(current.capacity * 2 , MAX_BLOCK_SIZE);
      new_size = std::max(new_size,size);
      blocks_.emplace_back(new_size);
      write_pos_ = 0;
      available = new_size;
    }

    size_t copy_size = std::min(remaining,available);
    memcpy(static_cast<char*>(blocks_.back().data)+write_pos_,src,copy_size);

    write_pos_ += copy_size;
    blocks_.back().size = write_pos_;
    src += copy_size;
    remaining -= copy_size;
    total_size_ += copy_size;
  }
}

//返回整体大小
size_t BufferBlock::readableBytes() const{
  return total_size_;
}                                

//清空block
void BufferBlock::clear(){
  blocks_.clear();
  blocks_.emplace_back(INITIAL_BLOCK_SIZE);
  total_size_ = read_pos_ = write_pos_ =check_pos_= 0;
}                                

const char *BufferBlock::peek()const{
  return (char*)blocks_[0].data + read_pos_;
}

void BufferBlock::peekFromBlock(char* dest,size_t n)const{
  if(n == 0)return;
  if(readableBytes()<n) return;
  //读第一块内存块的时候需要判断当前位置，之后就不用判断了
  size_t copied = 0;
  size_t available = 0;
  size_t pos = 0;
  size_t tmp_read_pos = read_pos_;
  int index = 0;
  for(const auto&block:blocks_){
    if (copied >= n) break;

    if(index==0){
      available = block.size - tmp_read_pos;
      pos =tmp_read_pos;
    }else{
      available = block.size;
      pos = 0;
    }

    size_t to_copy = std::min(n-copied,available);
    memcpy(dest+copied , static_cast<char*>(block.data)+pos,to_copy);
    
    copied +=to_copy;
    if (index == 0) {
      tmp_read_pos += to_copy;
    }
    index++;
  }
}

//用于移动read_pos_
void BufferBlock::consumeBytes(size_t size){
  if(size==0)return;

  size_t consumed = 0;
  while(consumed < size && !blocks_.empty()){
    Block &current = blocks_.front();
    size_t available = current.size -read_pos_;
    size_t to_consume = std::min(size-consumed,available);

    consumed += to_consume;
    read_pos_ += to_consume;

    if(read_pos_ == current.size){
      blocks_.erase(blocks_.begin());
      read_pos_ = 0;
    }
  }  
  total_size_ -= consumed;
  check_pos_ = read_pos_;
}

void BufferBlock::readBytes(char* dest, size_t n){
  if(n==0) return;
  size_t copied = 0;

  while(copied < n){
    Block& current = blocks_.front();

    size_t available = current.size - read_pos_;
    size_t to_copy = std::min(n-copied,available);

    memcpy(dest+copied, static_cast<char*>(current.data)+read_pos_,to_copy);

    copied += to_copy;
    read_pos_ += to_copy;

    if(read_pos_ == current.size){
      blocks_.erase(blocks_.begin());
      read_pos_=0;
    }
  }

  total_size_ -= n;
  check_pos_ = read_pos_;
}

void BufferBlock::erase(int len){
  if(len<=0 || len>total_size_) return;

  if(len==total_size_){ 
    clear(); 
    return;
  }

  int length=len;
  while(length>0 && !blocks_.empty()){
    Block& current = blocks_.front();
    size_t available = current.size - read_pos_;

    if(available <= length){
      length -= available;
      blocks_.erase(blocks_.begin());
      read_pos_ = 0;
    }else{
      read_pos_ += length;
      break;
    }
  }
  total_size_ -= len;
  check_pos_ = read_pos_;
}


size_t BufferBlock::getIOVecs(struct iovec* iovs, size_t max_count, size_t start_pos) const{
  if(blocks_.empty() || max_count ==0) return 0;

  size_t count = 0;
  size_t tmp_read_pos = start_pos;

  for(const auto& block:blocks_){
    if(count>= max_count) break;

    size_t available = block.size-tmp_read_pos;
    if(available>0){
      iovs[count].iov_base = static_cast<char*>(block.data) + tmp_read_pos;
      iovs[count].iov_len = available;
      count++;
    }
    tmp_read_pos =0;
  }

  return count;
}

std::string BufferBlock::bufferToString() {
  std::string str;
  str.resize(readableBytes());
  peekFromBlock(const_cast<char*>(str.data()),readableBytes());
  return str;
}