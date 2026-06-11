document.addEventListener('DOMContentLoaded', () => {
    // DOM Elements
    const scheduleForm = document.getElementById('schedule-form');
    const actionBtns = document.querySelectorAll('.action-btn');
    const actionInput = document.getElementById('action-input');
    const timeTypeSelect = document.getElementById('time-type');
    const minutesGroup = document.getElementById('minutes-group');
    const targetGroup = document.getElementById('target-group');
    const formMessage = document.getElementById('form-message');
    const submitBtn = document.querySelector('.submit-btn');
    const btnText = document.querySelector('.btn-text');
    const loader = document.querySelector('.loader');
    const scheduleList = document.getElementById('schedule-list');
    const refreshBtn = document.getElementById('refresh-btn');

    // UI Logic: Toggle Action Buttons
    actionBtns.forEach(btn => {
        btn.addEventListener('click', () => {
            actionBtns.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            actionInput.value = btn.dataset.action;
        });
    });

    // UI Logic: Toggle Time Input Type
    timeTypeSelect.addEventListener('change', (e) => {
        if (e.target.value === 'minutes') {
            minutesGroup.classList.remove('hidden');
            targetGroup.classList.add('hidden');
        } else {
            minutesGroup.classList.add('hidden');
            targetGroup.classList.remove('hidden');
        }
    });

    // Handle Form Submit
    scheduleForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        
        // Form state loading
        setLoading(true);
        showMessage('', '');

        const action = actionInput.value;
        const timeType = timeTypeSelect.value;
        
        const payload = { Action: action };

        if (timeType === 'minutes') {
            const mins = document.getElementById('delay-minutes').value;
            if (!mins) {
                showMessage('請輸入延遲分鐘數', 'error');
                setLoading(false);
                return;
            }
            payload.DelayMinutes = parseInt(mins);
        } else {
            const time = document.getElementById('target-time').value;
            if (!time) {
                showMessage('請選擇目標時間', 'error');
                setLoading(false);
                return;
            }
            payload.TargetTime = time;
        }

        try {
            const response = await fetch('/api/schedule', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(payload)
            });

            const result = await response.json();

            if (response.ok) {
                showMessage(result.message || '排程設定成功！', 'success');
                // Reset inputs
                document.getElementById('delay-minutes').value = '';
                document.getElementById('target-time').value = '';
                // Reload list
                loadSchedules();
            } else {
                showMessage(result.message || '排程失敗，請稍後再試', 'error');
            }
        } catch (error) {
            showMessage('連線發生錯誤', 'error');
            console.error(error);
        } finally {
            setLoading(false);
        }
    });

    // Refresh list manually
    refreshBtn.addEventListener('click', loadSchedules);

    // Load Schedules from API
    async function loadSchedules() {
        try {
            const response = await fetch('/api/schedules');
            if (!response.ok) throw new Error('Failed to load');
            
            const list = await response.json();
            renderSchedules(list);
        } catch (error) {
            console.error('Error loading schedules:', error);
            scheduleList.innerHTML = '<div class="empty-state" style="color:var(--danger-color)">無法載入排程</div>';
        }
    }

    // Render Schedules to DOM
    function renderSchedules(list) {
        if (!list || list.length === 0) {
            scheduleList.innerHTML = '<div class="empty-state">目前沒有排程</div>';
            return;
        }

        scheduleList.innerHTML = '';
        
        list.forEach(item => {
            const executeTime = new Date(item.executeAt);
            const isTurnOn = item.action === 'turn_on';
            
            const card = document.createElement('div');
            card.className = 'schedule-card';
            card.innerHTML = `
                <div class="schedule-info">
                    <div class="schedule-action">
                        <span class="badge ${isTurnOn ? 'badge-on' : 'badge-off'}">${isTurnOn ? '開冷氣' : '關冷氣'}</span>
                    </div>
                    <div class="schedule-time">
                        預計執行: ${formatDate(executeTime)}
                    </div>
                </div>
                <button class="btn btn-danger delete-btn" data-id="${item.messageId}">取消</button>
            `;
            
            scheduleList.appendChild(card);
        });

        // Add delete event listeners
        document.querySelectorAll('.delete-btn').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const id = e.target.dataset.id;
                const originalText = e.target.innerText;
                e.target.innerText = '取消中...';
                e.target.disabled = true;
                
                try {
                    const res = await fetch(`/api/schedule/${id}`, { method: 'DELETE' });
                    if (res.ok) {
                        loadSchedules(); // Reload on success
                    } else {
                        throw new Error('Delete failed');
                    }
                } catch (error) {
                    alert('取消失敗，請稍後再試');
                    e.target.innerText = originalText;
                    e.target.disabled = false;
                }
            });
        });
    }

    // Helpers
    function setLoading(isLoading) {
        submitBtn.disabled = isLoading;
        if (isLoading) {
            btnText.classList.add('hidden');
            loader.classList.remove('hidden');
        } else {
            btnText.classList.remove('hidden');
            loader.classList.add('hidden');
        }
    }

    function showMessage(msg, type) {
        if (!msg) {
            formMessage.className = 'message hidden';
            formMessage.innerText = '';
            return;
        }
        formMessage.className = `message ${type}`;
        formMessage.innerText = msg;
    }

    function formatDate(date) {
        const d = new Date(date);
        const mm = String(d.getMonth() + 1).padStart(2, '0');
        const dd = String(d.getDate()).padStart(2, '0');
        const HH = String(d.getHours()).padStart(2, '0');
        const min = String(d.getMinutes()).padStart(2, '0');
        return `${mm}/${dd} ${HH}:${min}`;
    }

    // Initial load
    loadSchedules();
});
