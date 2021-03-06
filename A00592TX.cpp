/**********************************************************************
 * Arduino code to sniff the Acurite 00592TX wireless temperature
 * probe output data stream.
 *
 * Ideas on decoding protocol and prototype code from
 * Ray Wang (Rayshobby LLC) http://rayshobby.net/?p=8998
 *
 * Sniff the AcuRite model 00771W Indoor / Outdoor Thermometer
 * wireless data stream and display the results.
 * http://www.acurite.com/media/manuals/00754-instructions.pdf
 *
 * Code based on Ray Wang's humidity_display.ino source.
 * Heavily modified by Brad Hunting.
 *
 * The 00592TX wireless temperature probe contains a 433 MHz
 * wireless transmitter. The temperature from the probe is
 * sent approximately every 16 seconds.
 *
 * The 00592TX typically only sends one SYNC pulse + DATA stream
 * per temperature reading. Infrequently two sync/data streams
 * are sent during the same transmit window but that seems to 
 * be the exception.
 *
 * Ray Wang's code is for a different model of probe, one that 
 * transmits both temperature and humidity. Ray' code relies on 
 * two sync streams with a preceeding delay. 
 * 
 * The 00592TX usually starts the data sync bits right after
 * the RF sync pulses which are random length and polarity.
 *
 * Do not rely on a dead/mark time at the beginning of the 
 * data sync stream.
 *
 * The 00592TX first emits a seemingly random length string of 
 * random width hi/lo pulses, most like to provide radio
 * radio synchronization.
 *
 * The probe then emits 4 data sync pulses of approximately 50% 
 * duty cycle and 1.2 ms period. The sync pulses start with a 
 * high level and continue for 4 high / low pulses.
 *
 * The data bits immediately follow the fourth low of the data
 * sync pulses. Data bits are sent every ~0.61 msec as:
 *
 * 1 bit ~0.4 msec high followed by ~0.2 msec low
 * 0 bit ~0.2 msec high followed by ~0.4 msec low
 *
 * The 00592TX sends the 4 sync pulses followed by
 * 7 bytes of data equalling 56 bits.
 *
 * The code below works by receiving a level change interrupt 
 * on each changing edge of the data stream from the RF module
 * and recording the time in uSec between each edge.
 *
 * 8 measured hi and lo pulses in a row, 4 high and 4 low, of 
 * approximately 600 uSec each constitue a sync stream.
 *
 * The remaining 56 bits of data, or 112 edges, are measured
 * and converted to 1s and 0s by checking the high to low
 * pulse times.
 *
 * The first 4 pulses, or 8 edges, are the sync pulses followed
 * by the 56 bits, or 112 edges, of the data pulses.
 *
 * We measure 8 sync edges followed by 112 data edges so the 
 * time capture buffer needs to be at least 120 long.
 *
 * This code presently does not calculate the checksum of
 * the data stream. It simply displays the results of what was 
 * captured from the RF module.
 *
 * The data stream is 7 bytes long.
 * The first and second bytes are unique address bytes per probe.
 *   The upper two bits of the first byte are the probe channel indicator:
 *   11 = channel A
 *   10 = channel B
 *   00 = channel C
 *   The remaining 6 bits of the first byte and the 8 bits of the second
 *   byte are a unique identifier per probe.
 * The upper nybble of the third byte carries the remote probe low battery indication.
 *   When the remote probe batteries are fresh, voltage above 2.5V, the third byte is 0x44.
 *   When the remote probe batteries get low, below 2.4V, the third byte changes to 0x84.
 * The fourth byte continues to stay at 0x90 for all conditions.
 * The next two bytes are the temperature. The temperature is encoded as the
 *   lower 7 bits of both bytes with the most significant bit being an
 *   even parity bit.  The MSB will be set if required to insure an even
 *   number of bits are set to 1 in the byte. If the least significant
 *   seven bits have an even number of 1 bits set the MSB will be 0,
 *   otherwise the MSB will be set to 1 to insure an even number of bits.
 * The last byte is a simple running sum, modulo 256, of the previous 6 data bytes.
 */

#include "A00592TX.h"

 // The pulse durations are the measured time in micro seconds between pulse edges.
static unsigned long pulseDurations[RING_BUFFER_SIZE];
static unsigned int syncIndex       = 0;        // index of the last bit time of the sync signal
static unsigned int dataIndex       = 0;        // index of the first bit time of the data bits (syncIndex+1)
static bool         syncFound       = false;    // true if sync pulses found
static bool         received        = false;    // true if sync plus enough bits found
static unsigned int changeCount     = 0;        // count edges of data
static const byte   interruptPin    = 3;



//--------------------------------------------------------------
//--- data structures for returning temperature data -----------
//--------------------------------------------------------------

// raw byte stream from sensors
static struct acurite_00592TX rawProbeData;
 
 // formatted data pulled from raw sensor data
struct sensorTemperatureData sensorData[ _numSensors ];

// number of sensor / data structures that _I_ have in my house
// static const uint8_t _numSensors = 6; // I happen to have 6 sensor probes

// The binary addresses assigned by the manufacturer for the sensors
// _I_ have in my house (change to your sensor addressses as needed)
// static const uint16_t SENSORID01 = 0x0C34;
// static const uint16_t SENSORID02 = 0x1E09;
// static const uint16_t SENSORID03 = 0x26ED;
// static const uint16_t SENSORID04 = 0x36E7;
// static const uint16_t SENSORID05 = 0x0604;
// static const uint16_t SENSORID06 = 0x386C;

static uint16_t probeIdArray[_numSensors] = 
{ SENSORID01, SENSORID02, SENSORID03, SENSORID04, SENSORID05, SENSORID06 };

static uint8_t CRC = 0;

// reduced sensor status
// static const uint8_t BATTERY_LOW_MASK = 0xC0;
// static const uint8_t BATTERY_LOW_VAL  = 0x80;
// static const uint8_t BATTERY_OK_VAL   = 0x40;
// static const uint8_t BATTERY_LOW      = 0x80;

//--------------------------------------------------------------
//--------------------------------------------------------------


/*-------------------------------------------------------------------
 * ------------- PrintHex8() ----------------------------------------
 * helper code to print formatted hex 
 * prints 8-bit data in hex
 */
void PrintHex8(uint8_t *data, uint8_t length)
{
    char tmp[length*2+1];
    byte first;
    int j = 0;
    for (uint8_t i = 0; i < length; i++) 
    {
        first = (data[i] >> 4) | 48;
        if (first > 57) tmp[j] = first + (byte)39;
        else tmp[j] = first ;
        j++;

        first = (data[i] & 0x0F) | 48;
        if (first > 57) tmp[j] = first + (byte)39; 
        else tmp[j] = first;
        j++;
    }
    tmp[length*2] = 0;
    Serial.print(tmp);
}

/*-------------------------------------------------------------------
 * ------------------ isSync() --------------------------------------
 * Look for the sync pulse train, 4 high-low pulses of
 * 600 uS high and 600 uS low.
 * idx is index of last captured bit duration.
 * Search backwards 8 times looking for 4 pulses
 * approximately 600 uS long.
 */
bool isSync(unsigned int idx) 
{
   // check if we've received 4 pulses of matching timing
   for( int i = 0; i < SYNCPULSEEDGES; i += 2 )
   {
      unsigned long t1 = pulseDurations[(idx+RING_BUFFER_SIZE-i) % RING_BUFFER_SIZE];
      unsigned long t0 = pulseDurations[(idx+RING_BUFFER_SIZE-i-1) % RING_BUFFER_SIZE];    
      
      // any of the preceeding 8 pulses are out of bounds, short or long,
      // return false, no sync found
      if( t0<(SYNC_HIGH-100) || t0>(SYNC_HIGH+100) ||
          t1<(SYNC_LOW-100)  || t1>(SYNC_LOW+100) )
      {
         return false;
      }
   }
   return true;
}

//----------------------------- handler() ------------------------
/* Interrupt 1 handler 
 * Tied to pin 3 INT1 of arduino.
 * Set to interrupt on edge (level change) high or low transition.
 * Change the state of the Arduino LED (pin 13) on each interrupt. 
 * This allows scoping pin 13 to see the interrupt / data pulse train.
 */
static void handler_rf433() 
{
   static unsigned long duration = 0;
   static unsigned long lastTime = 0;
   static unsigned int ringIndex = 0;
   static unsigned int syncCount = 0;
   static unsigned int bitState  = 0;

   bitState = digitalRead(DATAPIN);
   //digitalWrite(13, bitState); // toggle Arduino pin 13 which is typically the LED pin

   // ignore if we haven't finished processing the previous 
   // received signal in the main loop.
   if( received == true )
   {
      return;
   }

   // calculating timing since last change
   long time = micros();
   duration = time - lastTime;
   lastTime = time;

   // Known error in bit stream is runt/short pulses.
   // If we ever get a really short, or really long, 
   // pulse we know there is an error in the bit stream
   // and should start over.
   if( (duration > (PULSE_LONG+100)) || (duration < (PULSE_SHORT-100)) )
   {
      received = false;
      syncFound = false;
      changeCount = 0;  // restart looking for data bits
   }

   // store data in ring buffer
   ringIndex = (ringIndex + 1) % RING_BUFFER_SIZE;
   pulseDurations[ringIndex] = duration;
   changeCount++; // found another edge

   // detect sync signal
   if( isSync(ringIndex) )
   {
      syncFound = true;
      changeCount = 0;  // restart looking for data bits
      syncIndex = ringIndex;
      dataIndex = (syncIndex + 1)%RING_BUFFER_SIZE;
   }

   // If a sync has been found the start looking for the
   // DATABITSEDGES data bit edges.
   if( syncFound )
   {
      // if not enough bits yet, no message received yet
      if( changeCount < DATABITSEDGES )
      {
         received = false;
      }
      else if( changeCount > DATABITSEDGES )
      {
        // if too many bits received then reset and start over
         received = false;
         syncFound = false;
      }
      else
      {
         received = true;
		 // disable interrupt to avoid new data corrupting the buffer
         detachInterrupt(digitalPinToInterrupt(interruptPin));
      }
   }
}

//---------------- convertTimingToBit() -------------------------------
/*
 * Convert pulse durations to bits.
 * 
 * 1 bit ~0.4 msec high followed by ~0.2 msec low
 * 0 bit ~0.2 msec high followed by ~0.4 msec low
 */
int convertTimingToBit(unsigned int t0, unsigned int t1) 
{
   if( t0 > (BIT1_HIGH-100) && t0 < (BIT1_HIGH+100) && t1 > (BIT1_LOW-100) && t1 < (BIT1_LOW+100) )
   {
      return 1;
   }
   else if( t0 > (BIT0_HIGH-100) && t0 < (BIT0_HIGH+100) && t1 > (BIT0_LOW-100) && t1 < (BIT0_LOW+100) )
   {
      return 0;
   }
   return -1;  // undefined
}

//---------------- ageStaleData() -------------------------------
/*
 * Mark data that is more than N seconds old as NOT FRESH
 */
void ageStaleData() 
{
    // compare timestamp of data to stale timeout
    int i = 0;
    for( i = 0; i < _numSensors; i++ )
    {
        if( (sensorData[i].timestamp + SENSOR_STALE_DATA_TIMEOUT) < (millis() / 1000) )
        {
            sensorData[i].status &= ~SENSOR_DATA_FRESH_MASK;
        }
    }
}

/***********************************************************************/
//#define PRINT_DATA_ARRAY
// #define PRINT_NEW_DATA

//---------------- setup() -------------------------------------------
void setup592()
{
   pinMode(DATAPIN, INPUT);             // data interrupt input
   attachInterrupt(digitalPinToInterrupt(interruptPin), handler_rf433, CHANGE);
   pinMode(SQUELCHPIN, OUTPUT);         // data squelch pin on radio module
   digitalWrite(SQUELCHPIN, HIGH);      // UN-squelch data

   memset( sensorData, 0, sizeof(sensorData));
  // pre-fill various values into the data structure.
  for(uint8_t i = 0; i < _numSensors; i++)
  {
    sensorData[i].id = i+1;
  }
}

/***********************************************************************/
//-------------------- loop592() ----------------------------------------------
/*
 * Main Loop
 * Wait for received to be true, meaning a sync stream plus
 * all of the data bit edges have been found.
 * Convert all of the pulse timings to bits and calculate
 * the results.
 */
void loop592()
{
   if( received == true )
   {
      // convert bits to bytes
      unsigned int startIndex, stopIndex, ringIndex;
      uint8_t dataBytes[DATABYTESCNT];
      // clear the data bytes array
      for( int i = 0; i < DATABYTESCNT; i++ )
      {
        dataBytes[i] = 0;
      }
        
      ringIndex = (syncIndex+1)%RING_BUFFER_SIZE;

      for( int i = 0; i < DATABITSCNT; i++ )
      {
         int bit = convertTimingToBit( pulseDurations[ringIndex%RING_BUFFER_SIZE], 
                                       pulseDurations[(ringIndex+1)%RING_BUFFER_SIZE] );
                                       
         if( bit < 0 )
         {  
            Serial.println("Bit Timing : Decoding error.");
            // reset flags to allow next capture
            received = false;
            syncFound = false;
            // re-enable interrupt
            attachInterrupt(digitalPinToInterrupt(interruptPin), handler_rf433, CHANGE);
            return;      // exit due to error
         }
         else
         {
            dataBytes[i/8] |= bit << (7-(i%8));
         }
         
         ringIndex += 2;
      }

      // calculate CRC as sum of first 6 bytes
      CRC = 0; 
      for( int i = 0; i < DATABYTESCNT-1; i++ )
      {
        CRC += dataBytes[i]; 
      }            

      // overlay typed stucture over raw bytes 
      acurite_00592TX * acurite_data = (acurite_00592TX *)&dataBytes[0];

      // CRC ERROR in received data, ignore
      if( CRC != acurite_data->crc )
      {
            Serial.println("Sensor Data CRC : CRC error.");
            // reset flags to allow next capture
            received = false;
            syncFound = false;
            // re-enable interrupt
            attachInterrupt(digitalPinToInterrupt(interruptPin), handler_rf433, CHANGE);
            return;      // exit due to error
      }
      
      // fill in sensor data array
      uint16_t hexID = acurite_data->id_high * 256 + acurite_data->id_low;
      uint8_t  id = _numSensors+1; // preset to illegal

      // find which sensor id, search sendor id array
      for( int i = 0; i < _numSensors; i++ )
      {
          if( hexID == probeIdArray[i] )
          {
                id = i;
          }
      }

      if( id > _numSensors )
      {
            Serial.println("Sensor ID : out of bounds error.");
            // reset flags to allow next capture
            received = false;
            syncFound = false;
            // re-enable interrupt
            attachInterrupt(digitalPinToInterrupt(interruptPin), handler_rf433, CHANGE);
            return;      // exit due to error
      }

      sensorData[id].id = id+1;
      
      // check for a low battery indication
      if( (acurite_data->status & SENSOR_BATTERY_LOW_MASK) == SENSOR_BATTERY_LOW_VAL )
      {
         sensorData[id].status |= SENSOR_BATTERY_LOW;
      }
      else
      {
        sensorData[id].status &= ~SENSOR_BATTERY_LOW;
      }
      
      // set sensor data as fresh
      sensorData[id].status |= (SENSOR_DATA_FRESH_VAL & SENSOR_DATA_FRESH_MASK);

      // extract temperature value
      uint16_t temperature = 0;
      sensorData[id].temperature = 0;
      
      // data bytes have already been decoded
      // 7 bits are significant, 8th bit is even parity bit
      // only shift by 7 bits because low byte is 7 bits of data
      temperature = ((acurite_data->temperature_high) & 0x7F) << 7;
      temperature += (acurite_data->temperature_low) & 0x7F;
      // temperature is offset by 1024 (= 0x400 = b0100 0000 0000)
      sensorData[id].temperature = (uint16_t)((temperature-1024)+0.5);
      
      // update timestamp of new data
      sensorData[id].timestamp = millis() / 1000;  // convert milli-seconds into seconds
      
      
#ifdef PRINT_NEW_DATA
        Serial.print("id = ");
        Serial.print(sensorData[id].id);
        Serial.print(", status = ");
        Serial.print(sensorData[id].status, HEX);
        Serial.print(", temperature = ");
        Serial.print(sensorData[id].temperature);
        Serial.print(", time = ");
        Serial.println(sensorData[id].timestamp);
#endif // PRINT_NEW_DATA
      
#ifdef PRINT_DATA_ARRAY
      for( int i = 0; i < _numSensors; i++ )
      {
        Serial.print("id = ");
        Serial.print(sensorData[i].id);
        Serial.print(", status = ");
        Serial.print(sensorData[i].status, HEX);
        Serial.print(", temperature = ");
        Serial.print(sensorData[i].temperature);
        Serial.print(", time = ");
        Serial.println(sensorData[i].timestamp);
      }
#endif // PRINT_DATA_ARRAY

      // clear sensor data received flag to read next data      
      received = false;
      syncFound = false;

      // re-enable interrupt
      attachInterrupt(digitalPinToInterrupt(interruptPin), handler_rf433, CHANGE);
   } // new data received
   ageStaleData(); // flag data that has not been refhreshed recently
} // loop592
