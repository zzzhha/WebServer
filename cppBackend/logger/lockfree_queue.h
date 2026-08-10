#pragma once
#include <atomic>
#include <vector>
#include <iterator>

template<typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T val) : data(std::move(val)), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    
public:
    LockFreeQueue() {
        Node* dummy = new Node(T());
        head_.store(dummy);
        tail_.store(dummy);
    }
    
    ~LockFreeQueue() {
        while (Node* old_head = head_.load()) {
            head_.store(old_head->next.load());
            delete old_head;
        }
    }
    
    void Push(T item) {
        Node* new_node = new Node(std::move(item));
        Node* old_tail;
        while(true) {
            old_tail = tail_.load();
            Node* next = old_tail->next.load();
            if(old_tail == tail_.load()) {
                if(next == nullptr) {
                    if(old_tail->next.compare_exchange_weak(next, new_node)) {
                        break;
                    }
                } else {
                    tail_.compare_exchange_weak(old_tail, next);
                }
            }
        }
        tail_.compare_exchange_weak(old_tail, new_node);
    }
    
    bool Pop(T& result) {
        Node* old_head;
        while(true) {
            old_head = head_.load();
            Node* old_tail = tail_.load();
            Node* next = old_head->next.load();
            if(old_head == head_.load()) {
                if(old_head == old_tail) {
                    if(next == nullptr) return false;
                    tail_.compare_exchange_weak(old_tail, next);
                } else {
                    // 低危修复：先拷贝数据、CAS 成功后才作为结果返回。
                    // 旧实现先 std::move(next->data) 再 CAS：CAS 失败时
                    // next 仍在队列中，但其数据已被移走 → 数据丢失/损坏。
                    result = next->data;
                    if(head_.compare_exchange_weak(old_head, next)) {
                        delete old_head;
                        return true;
                    }
                }
            }
        }
    }
    
    // 说明（低危问题）：GetSizeApprox 仅当无并发 Pop 时安全——并发删除节点时
    // 遍历可能触及已释放节点（UAF）。当前未启用该队列（Logger 使用 std::queue），
    // 如需并发统计请改用带 hazard pointer / epoch 的实现。
    size_t GetSizeApprox() const {
        size_t count = 0;
        Node* node = head_.load();
        while (node) {
            Node* next = node->next.load();
            if (next) count++;
            node = next;
        }
        return count;
    }
    
    bool Empty() const {
        return head_.load() == tail_.load();
    }
    
    void PopBatch(std::vector<T>& batch, size_t max_size) {
        batch.clear();
        Node* old_head;
        while(true) {
            old_head = head_.load();
            Node* old_tail = tail_.load();
            Node* next = old_head->next.load();
            
            if(old_head == head_.load()) {
                if(old_head == old_tail) {
                    if(next == nullptr) return;
                    tail_.compare_exchange_weak(old_tail, next);
                } else {
                    size_t count = 0;
                    Node* current = next;
                    Node* new_tail = old_head;
                    std::vector<T> staged;  // 低危修复：CAS 成功前先暂存拷贝，
                                            // 避免 CAS 失败时已把节点数据移走
                    staged.reserve(max_size);
                    
                    while (current && count < max_size) {
                        staged.push_back(current->data);
                        new_tail = current;
                        current = current->next.load();
                        count++;
                    }
                    
                    if(head_.compare_exchange_weak(old_head, new_tail)) {
                        batch.insert(batch.end(),
                                     std::make_move_iterator(staged.begin()),
                                     std::make_move_iterator(staged.end()));
                        Node* to_delete = old_head->next.load();
                        while (to_delete != new_tail) {
                            Node* next_delete = to_delete->next.load();
                            delete to_delete;
                            to_delete = next_delete;
                        }
                        delete old_head;
                        return;
                    }
                }
            }
        }
    }
};
