import serial
import serial.tools.list_ports
import threading
import time
import tkinter as tk
from tkinter import scrolledtext
import json
import math
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from PIL import Image, ImageTk

# Serial config
SERIAL_PORT = 'COM5'
BAUD_RATE = 115200
ACK_TIMEOUT = 2  # seconds to wait for ACK before retrying
MAX_RETRIES = 3  # max number of retries per message
sd_mode = True  # set to True for SD mode, False for normal mode

message_queue = []
message_lock = threading.Lock()
awaiting_ack = False
current_message = None
ack_received_time = None
retry_count = 0

timestamps = True

ui_labels = {}

graph_data = {
    'time': [],
    'block_temperature': [],
    'target_block_temp': [],
    'cap_temperature': [],
    'target_cap_temp': [],
    'ax': None,
    'fig': None,
    'canvas': None,
    'start_time': time.time()
}

def handle_sync(message, sd_mode):
    message = message.strip()
    if message.lower() == "syn":
        return "syn ack\n" if sd_mode else "syn ack no_sd\n"
    return None

def update_graph(data_dict):

    t = time.time() - graph_data['start_time']
    graph_data['time'].append(t)

    # Append values, defaulting to the previous value or 0 if not present
    for key in ['block_temperature', 'target_block_temp', 'cap_temperature', 'target_cap_temp']:
        if key in data_dict:
            graph_data[key].append(float(data_dict[key]))
        else:
            last_val = graph_data[key][-1] if graph_data[key] else 0
            graph_data[key].append(last_val)

    #limit max length of data
    if len(graph_data['time']) > 1200:
        for key in ['time', 'block_temperature', 'target_block_temp', 'cap_temperature', 'target_cap_temp']:
            graph_data[key] = graph_data[key][-1200:]


    # Update the graph
    ax = graph_data['ax']
    ax.clear()
    ax.set_title("Temperature Over Time")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Temperature (°C)")

    ax.plot(graph_data['time'], graph_data['block_temperature'], label='Block Temp', color='red')
    ax.plot(graph_data['time'], graph_data['target_block_temp'], label='Block Target Temp', color='orange', linestyle='--')
    ax.plot(graph_data['time'], graph_data['cap_temperature'], label='Cap Temp', color='blue')
    ax.plot(graph_data['time'], graph_data['target_cap_temp'], label='Cap Target Temp', color='green', linestyle='--')

    ax.legend(loc='upper left')
    ax.set_ylim(0, 120)
    
    graph_data['canvas'].draw()



def update_variables_panel(data_dict):

    global arrived_at_temp_needed

    #parse incoming data and update the UI
    for key, value in data_dict.items():
        if key in ui_labels:
            ui_labels[key]['text'] = f"{key}: {value}"
            if key == "temp_reached":
                arrived_at_temp_needed = value
        else:
            label = tk.Label(variables_frame, text=f"{key}: {value}", anchor='w', font=('Arial', 15))
            label.pack(fill='x', padx=5)
            ui_labels[key] = label

def read_from_serial(ser, output_text):
    global awaiting_ack, current_message, ack_received_time, retry_count

    while True:
        if ser.in_waiting > 0:
            incoming_data = ser.readline().decode('utf-8', errors='ignore').strip()


            # Handle incoming messages
            response = handle_sync(incoming_data, sd_mode) #check for sync message
            
            if response:
                ser.write(response.encode('utf-8'))
                output_text.config(state=tk.NORMAL)

                #add a timestamp if enabled
                if timestamps:
                    output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > Sent: {response}")
                else:
                    output_text.insert(tk.END, f"Sent: {response}")
                output_text.see(tk.END)
                output_text.config(state=tk.DISABLED)

            # Check for ACK message
            if incoming_data.lower() == "ack" and awaiting_ack:
                awaiting_ack = False
                ack_received_time = time.time()
                retry_count = 0
                current_message = None
                output_text.config(state=tk.NORMAL)
                if timestamps:
                    output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > ACK received\n", 'green')
                else:
                    output_text.insert(tk.END, "ACK received\n", 'green')
                output_text.see(tk.END)
                output_text.config(state=tk.DISABLED)

            try:
                # Attempt to parse incoming data as JSON
                # If successful, update the variables panel and graph
                data_dict = json.loads(incoming_data)
                if isinstance(data_dict, dict):
                    root.after(0, update_variables_panel, data_dict)
                    root.after(0, update_graph, data_dict)
            except json.JSONDecodeError:

                # If JSON parsing fails, treat it as a regular string and display it
                output_text.config(state=tk.NORMAL)
                if timestamps:
                    output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > {incoming_data}\n")
                else:
                    output_text.insert(tk.END, f"Received: {incoming_data}\n")
                output_text.see(tk.END)
                output_text.config(state=tk.DISABLED)

                pass

        # Handle outgoing messages with ACK verification
        if not awaiting_ack:
            #lock the message queue to prevent race conditions
            with message_lock:
                if message_queue:
                    current_message = message_queue.pop(0)
                    ser.write(current_message.encode('utf-8'))
                    # Display the sent message in the output text area
                    output_text.config(state=tk.NORMAL)
                    if timestamps:
                        output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > Sent: {current_message}")
                    else:
                        output_text.insert(tk.END, f"Sent: {current_message}")
                    output_text.see(tk.END)
                    output_text.config(state=tk.DISABLED)

                    awaiting_ack = True
                    ack_received_time = time.time()
                    retry_count = 1
        else: # handle waiting for ACK
            # Waiting for ACK: check timeout
            if time.time() - ack_received_time > ACK_TIMEOUT:
                if retry_count < MAX_RETRIES:
                    ser.write(current_message.encode('utf-8'))
                    output_text.config(state=tk.NORMAL)
                    if timestamps:
                        output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > Retrying ({retry_count}): {current_message}", 'orange')
                    else:
                        output_text.insert(tk.END, f"Retrying ({retry_count}): {current_message}", 'orange')
                    output_text.see(tk.END)
                    output_text.config(state=tk.DISABLED)
                    ack_received_time = time.time()
                    retry_count += 1
                else:
                    # Max retries reached
                    output_text.config(state=tk.NORMAL)
                    if timestamps:
                        output_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} > Failed to send after {MAX_RETRIES} retries: {current_message}", 'red')
                    else:
                        output_text.insert(tk.END, f"Failed to send after {MAX_RETRIES} retries: {current_message}", 'red')
                    output_text.see(tk.END)
                    output_text.config(state=tk.DISABLED)
                    awaiting_ack = False
                    current_message = None
                    retry_count = 0

        time.sleep(0.1)


def cycle_controller(output_frame):
    
    #messages to be sent at the start of the cycle
    startup_messages = ["heat_act=true", "target_temp_cap=110"]
    
    #messages to be sent in the cycle
    messages = ["target_temp_block=95", "target_temp_block=60", "target_temp_block=72"]
    intervals = [30, 30, 45]  # Seconds for each message
    arrived_at_temp = [True, True, True]  # Whether to wait for the target temperature
    max_cycles = 5  # Number of cycles to run

    cycle_thread = None
    eta_thread = None
    pause_flag = threading.Event()
    stop_flag = threading.Event()
    end_flag = threading.Event()

    # GUI Elements
    eta_label = tk.Label(output_frame, text="Elapsed: 0.0s", font=('Arial', 12))
    eta_label.pack(pady=5)

    time_label = tk.Label(output_frame, text="Timer: 0.0s \nNext in: 0.0s", font=('Arial', 12))
    time_label.pack(pady=5)

    last_message_var = tk.StringVar(value="Last Message Sent: None")
    last_message_label = tk.Label(output_frame, textvariable=last_message_var, font=('Arial', 12))
    last_message_label.pack(pady=5)

    next_message_var = tk.StringVar(value="Next Message: None")
    next_message_label = tk.Label(output_frame, textvariable=next_message_var, font=('Arial', 12))
    next_message_label.pack(pady=5)

    cycle_number_var = tk.StringVar(value=f"Cycle Number: 0 Out of {max_cycles}")
    cycle_number_label = tk.Label(output_frame, textvariable=cycle_number_var, font=('Arial', 12))
    cycle_number_label.pack(pady=5)

    info_label = tk.Label(output_frame, text="Press 'Start' to begin the cycle.", font=('Arial', 12))
    info_label.pack(pady=5)

    # Functions to update the UI
    def update_eta_display():
        start_time = time.time()
        while not stop_flag.is_set():
            if cycle_thread and cycle_thread.is_alive():
                elapsed = time.time() - start_time
                eta_label.config(text=f"Elapsed: {elapsed:.1f}s")
            else:
                eta_label.config(text="Elapsed: 0.0s")
            time.sleep(0.1)
    
    def update_timer_display(elapsed, remaining):
        time_label.config(text=f"Timer: {elapsed:.1f}s \nNext in: {remaining:.1f}s")

    def update_last_message(msg):
        last_message_var.set(f"Last Message Sent: {msg}")

    def update_cycle_number(num):
        cycle_number_var.set(f"Cycle Number: {num} Out of {max_cycles}")

    def update_next_message(msg):
        next_message_var.set(f"Next Message: {msg}")

    def update_info_label(msg):
        if end_flag.is_set():
            msg += "\n\nEnding cycle set..."
        info_label.config(text=msg)


    # main cycle function
    def cycle_messages():
        i = 0

        #send startup messages
        for message in startup_messages:
            send_message_cycle(message)
            time.sleep(0.5) 
            update_last_message(message)
            update_next_message(messages[0])

        while not stop_flag.is_set():
            
            current_interval = intervals[i % len(intervals)]

            #check if the cycle should go to end conditions
            if math.floor((i/len(messages)) + 1) >= max_cycles or end_flag.is_set():
                
                update_info_label("Ending... Final extension started.")
                
                send_message_cycle("target_temp_block=72")
                
                #pause interval (not optimal as it could be done in a function, but it works)
                start_time = time.time()
                paused_duration = 0
                pause_start = None
                current_interval = 10*60  # 10 minutes
                
                while True:
                    if stop_flag.is_set():
                        return

                    if pause_flag.is_set():
                        if pause_start is None:
                            pause_start = time.time()
                        time.sleep(0.1)
                        continue
                    else:
                        if pause_start is not None:
                            paused_duration += time.time() - pause_start
                            pause_start = None

                    elapsed = time.time() - start_time - paused_duration
                    remaining = max(0, current_interval - elapsed)
                    update_timer_display(elapsed, remaining)

                    if elapsed >= current_interval:
                        break
                    time.sleep(0.1)

                #cool down
                send_message_cycle("target_temp_block=0")
                send_message_cycle("target_temp_cap=0")

                update_info_label("Final extension completed. Cooling down.")

                #pause for cooling
                start_time = time.time()
                paused_duration = 0
                pause_start = None
                current_interval = 2*60  # 10 minutes
                
                while True:
                    if stop_flag.is_set():
                        return

                    if pause_flag.is_set():
                        if pause_start is None:
                            pause_start = time.time()
                        time.sleep(0.1)
                        continue
                    else:
                        if pause_start is not None:
                            paused_duration += time.time() - pause_start
                            pause_start = None

                    elapsed = time.time() - start_time - paused_duration
                    remaining = max(0, current_interval - elapsed)
                    update_timer_display(elapsed, remaining)

                    if elapsed >= current_interval:
                        break
                    time.sleep(0.1)
                
                #final message
                send_message_cycle("heat_act=false")

                stop_flag.set()

                update_info_label("Cycle completed. Press 'Start' to begin again.")
                return

            #check next message
            message = messages[i % len(messages)]

            #send message
            send_message_cycle(message)
            time.sleep(1) #needed to allow the arduino program to recalculate the arrived_at_temp variable, if needed add to it
            update_last_message(message)
            update_next_message(messages[(i + 1) % len(messages)])

            update_cycle_number(math.floor((i/len(messages)) + 1))

            # if needed, wait for the target temperature to be reached
            if arrived_at_temp[i % len(arrived_at_temp)]:
                while not arrived_at_temp_needed:
                    time.sleep(0.1)
                    if pause_flag.is_set():
                        update_info_label("Cycle paused. Press 'Resume' to continue.")
                    else:
                        update_info_label("Waiting for target temperature to be reached...")
                    
                    if stop_flag.is_set():
                        return
                    

                
                update_info_label("Holding at target temperature...")

            #pause interval
            start_time = time.time()
            paused_duration = 0
            pause_start = None

            while True:
                if stop_flag.is_set():
                    return

                if pause_flag.is_set():
                    if pause_start is None:
                        pause_start = time.time()
                    time.sleep(0.1)
                    continue
                else:
                    if pause_start is not None:
                        paused_duration += time.time() - pause_start
                        pause_start = None

                elapsed = time.time() - start_time - paused_duration
                remaining = max(0, current_interval - elapsed)
                update_timer_display(elapsed, remaining)

                if elapsed >= current_interval:
                    break
                time.sleep(0.1)

            
            
            i += 1

    #srart the cycle
    def start_cycle():
        
        nonlocal cycle_thread, eta_thread
        if cycle_thread is None or not cycle_thread.is_alive():
            stop_flag.clear()
            pause_flag.clear()
            end_flag.clear()
            end_button.config(state=tk.NORMAL)
            stop_button.config(state=tk.NORMAL)
            start_button.config(state=tk.DISABLED)
            pause_button.config(state=tk.NORMAL)
            eta_thread = threading.Thread(target=update_eta_display, daemon=True)
            eta_thread.start()
            cycle_thread = threading.Thread(target=cycle_messages, daemon=True)
            cycle_thread.start()

    #stop the cycle
    def stop_cycle():
        stop_flag.set()
        pause_flag.clear()
        end_flag.clear()
        start_button.config(state=tk.NORMAL)
        pause_button.config(state=tk.DISABLED)
        stop_button.config(state=tk.DISABLED)
        end_button.config(state=tk.NORMAL)
        update_info_label("Cycle stopped. Press 'Start' to begin again.")

    #pause/resume the cycle
    def pause_cycle():
        if pause_flag.is_set():
            pause_flag.clear()
            update_info_label("Holding at target temperature...")
        else:
            pause_flag.set()
            update_info_label("Cycle paused. Press 'Resume' to continue.")

    #end the cycle
    def end_cycle():
        end_flag.set()
        end_button.config(state=tk.DISABLED)


    # Buttons
    start_button = tk.Button(output_frame, text="Start", command=start_cycle, state=tk.NORMAL)
    pause_button = tk.Button(output_frame, text="Pause/Resume", command=pause_cycle, state=tk.DISABLED)
    stop_button = tk.Button(output_frame, text="Stop", command=stop_cycle, state=tk.DISABLED)
    end_button = tk.Button(output_frame, text="End", command=end_cycle, state=tk.DISABLED)

    start_button.pack(side=tk.LEFT, padx=5)
    pause_button.pack(side=tk.LEFT, padx=5)
    stop_button.pack(side=tk.LEFT, padx=5)
    end_button.pack(side=tk.LEFT, padx=5)

#add messages to the queue
def send_message_cycle(entry):

    msg = entry.strip()

    with message_lock:
        message_queue.append(msg + "\n")  # Add newline to simulate proper format

#add written message to the queue
def send_message(entry):
    msg = entry.get().strip()
    if msg:
        with message_lock:
            message_queue.append(msg + "\n")  # Add newline to simulate proper format
        entry.delete(0, tk.END)

def main(ser):
    global root, variables_frame

    #create tkinter elements
    root = tk.Tk()
    root.title("Termocycler Serial Communicator")
    ico = Image.open('images/icon.png')
    photo = ImageTk.PhotoImage(ico)
    root.wm_iconphoto(False, photo)
    root.geometry("1400x1000")

    main_frame = tk.Frame(root)
    main_frame.pack(fill='both', expand=False)

    side_control_frame = tk.Frame(main_frame, bd=2, relief='sunken', padx=10)
    side_control_frame.pack(side='left', fill='y', padx=5, pady=5)
    side_control_frame.pack_propagate(False)
    side_control_frame.config(width=400, height=650)

    console_frame = tk.Frame(main_frame)
    console_frame.pack(side='left', fill='both', expand=True, padx=5, pady=5)

    output_text = scrolledtext.ScrolledText(console_frame, wrap=tk.WORD)
    output_text.pack(padx=10, pady=10, fill='both', expand=True)
    output_text.pack_propagate(False)
    output_text.config(state=tk.DISABLED)

    variables_frame = tk.Frame(main_frame, bd=2, relief='sunken', padx=10)
    variables_frame.pack(side='right', fill='y', padx=5, pady=5)
    variables_frame.pack_propagate(False)
    variables_frame.config(width=300, height=650)

    tk.Label(variables_frame, text="Live Variables", font=('Arial', 12, 'bold')).pack()

    entry_frame = tk.Frame(root)
    entry_frame.pack(padx=10, pady=5)

    entry = tk.Entry(entry_frame, width=40)
    entry.pack(side=tk.LEFT, padx=(0, 5))
    entry.bind('<Return>', lambda event: send_message(entry))

    send_button = tk.Button(entry_frame, text="Send", command=lambda: send_message(entry))
    send_button.pack(side=tk.LEFT)

    # Create graph frame
    graph_frame = tk.Frame(root, bd=2, relief='sunken')
    graph_frame.pack(side='bottom', fill='y', padx=5, pady=5, expand=True)
    graph_frame.config(width=800, height=400)

    fig = Figure(figsize=(15, 2), dpi=100)
    ax = fig.add_subplot(111)
    ax.set_title("Temperature Over Time")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Temperature (°C)")

    canvas = FigureCanvasTkAgg(fig, master=graph_frame)
    canvas.draw()
    canvas.get_tk_widget().pack(fill='both', expand=True)

    # Store plot elements in global state
    graph_data['ax'] = ax
    graph_data['fig'] = fig
    graph_data['canvas'] = canvas

    thread = threading.Thread(target=read_from_serial, args=(ser, output_text), daemon=True)
    thread.start()
    thread2 = threading.Thread(target=cycle_controller, args=(side_control_frame,), daemon=True)
    thread2.start()

    root.mainloop()

if __name__ == "__main__":
    
    timeout = time.time() + 20
    while True:
        
        ports = serial.tools.list_ports.comports()

        for port in ports:
            if port.hwid == "USB VID:PID=2341:1002 SER=F412FA9C9F1C":
                SERIAL_PORT = port.device
                print(f"Found device on {SERIAL_PORT}")
                break
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            print(f"Connected to {SERIAL_PORT}")
            break
        except serial.SerialException:
            if time.time() > timeout:
                print(f"Failed to connect to {SERIAL_PORT} after 5 seconds.")
                exit(1)
            time.sleep(0.5)
    
    main(ser)
